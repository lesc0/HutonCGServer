# cg-streamer — CEF 문자발생기(CG) → RGA → MPP H.264 → MPEG-TS/UDP (RK3588)

HTML/CSS로 그린 방송 그래픽(자막·티커·이미지)을 CEF 오프스크린 렌더링으로 받아, 필요하면 mp4 영상과
합성한 뒤 RK3588 하드웨어(RGA 색변환·합성, MPP H.264 인코딩)로 1080p60 MPEG-TS/UDP를 송출한다.

## 구성
```
CGServer/
├─ src/
│  ├─ cg-streamer/       C++ 구현체
│  │  ├─ main.cpp         모드(--edit/--run), CEF 클라이언트, cefQuery 브릿지, UDP 제어, 인코딩 루프,
│  │  │                   UI 지터 버퍼, 로컬 미리보기 창(--preview, X11)
│  │  ├─ mpp_encoder.*    RGA(BGRA→NV12, 영상+UI 합성) + MPP H.264 인코더(1080p60 CBR 8Mbps) + 미리보기 BGRX 추출
│  │  ├─ video_source.*   mp4 → libavformat 분리 → MPP 하드웨어 디코딩 (영상 합성용)
│  │  ├─ hdmirx_source.*  HDMI 입력(rk_hdmirx, V4L2) → dmabuf 프레임 (라이브 소스 합성용)
│  │  └─ ts_muxer.*       libavformat MPEG-TS 먹서 (UDP/파일)
│  └─ editor/        원격/로컬 저작용 Node(Express) + React (신규, 진행 중)
├─ inc/              헤더(mpp_encoder.h, video_source.h, hdmirx_source.h, dma_heap_buf.h, ts_muxer.h)
├─ lib/              벤더 라이브러리(ffmpeg/mpp/rga/x11 include+lib, apt 패키지에서 추출) + lib_arm64.tgz
├─ bin/
│  ├─ cgctl.sh       실행 중 제어 (next/prev/goto/quit/status)
│  └─ web/           런타임 웹 리소스 (빌드 시 실행 파일 옆으로 복사됨)
│     ├─ editor.html GrapesJS 저작 화면 (페이지 단위)
│     ├─ player.html 실행용 플레이어 (로딩 완료 → ready 신호, 페이지별 영상 요청)
│     ├─ girsday.html <video> 데모 페이지 (시스템 Chromium에서 MPP 디코딩 확인용)
│     ├─ girsday.mp4 테스트 영상 (1080p H.264, git 제외)
│     ├─ cg.css      공유 애니메이션 (페이드/슬라이드/롤링)
│     └─ grapes.min.* GrapesJS 0.23.6 (오프라인 포함, 라이선스 파일 동봉)
├─ CMakeLists.txt, CMakePresets.json   (프리셋: rk3588, rk3588-accel)
└─ cef_server_build.md   Rockchip 패치 CEF 서버 빌드 가이드 (<video> MPP 디코딩용)
```

## 데이터 흐름
```
저작: editor.html → [저장] → cefQuery("save:") → project.json
실행: player.html ← cefQuery("load") ← project.json
      로딩 완료 → cefQuery("ready") → 이 시점부터 인코딩
[UI]  CEF OnPaint(BGRA) → UI 지터 버퍼(3장) → 60Hz 인코딩 틱마다 1장
[영상] (페이지에 "video" 가 있으면) mp4 → MPP 디코딩, 또는 HDMI 입력 → RGA 로 UI 와 합성
      → RGA(NV12) → MPP(H.264) → TsMuxer(mpegts) → UDP
```

## 환경 (실측: FriendlyElec CM3588 = RK3588, Debian 11 bullseye arm64)
```bash
sudo apt install build-essential ninja-build pkg-config git \
  libgtk-3-dev libnss3 libx11-xcb1 libxcomposite1 libxdamage1 libxrandr2 \
  libgbm1 libdrm2 libasound2 libcups2 libxss1 \
  librockchip-mpp-dev librga-dev libavformat-dev libavcodec-dev libavutil-dev
```
- **cmake 3.21 이상 필요**(프리셋). Debian 11 기본 3.18이면 Kitware 바이너리 설치:
  `/opt/cmake-3.30.5-linux-aarch64` + `/usr/local/bin/cmake` 링크 (현재 보드 설정).
- **CEF**: 공식 `cef_binary_130.1.16+g5a7e5ed+chromium-130.0.6723.117_linuxarm64_minimal`
  (보드의 Chromium 130과 맞춤, glibc 2.31에서 동작 확인) → `/opt/cef/` 에 풀고 `~/.bashrc`:
  ```bash
  export CEF_ROOT=/opt/cef/cef_binary_130.1.16+g5a7e5ed+chromium-130.0.6723.117_linuxarm64_minimal
  ```
- 사용자는 `video`, `render` 그룹이어야 한다(`/dev/mpp_service`, `/dev/rga`, `/dev/dma_heap/*` 접근).

## 빌드
```bash
cd /root/work/CGServer/src/cg-streamer   # CMakeLists.txt / CMakePresets.json 위치
cmake --preset rk3588            # 최초 1회 (CEF_ROOT 환경변수 사용)
cmake --build --preset rk3588    # → bin/cg-streamer (+ libcef.so, 리소스). 웹은 bin/web/ 을 그대로 사용
```
- 웹 파일(bin/web/)은 복사 없이 바로 반영된다(실행 파일 옆 web/).
- 실험 경로(`--accel`)는 `rk3588-accel` 프리셋 (아래 "실험" 참고).

## 실행
실행 파일 폴더에서 실행한다(옆의 `libcef.so`, `*.pak`, `web/` 사용).
```bash
cd /root/work/CGServer/build/rk3588/Release

# 송출 (기본 udp://127.0.0.1:1234)
./cg-streamer --run --project=/root/work/CGServer/build/image_project.json
./cg-streamer --run --project=... --udp=10.10.10.11:1234        # 유니캐스트
./cg-streamer --run --project=... --udp=239.1.1.1:1234          # 멀티캐스트
./cg-streamer --run --project=... --out=capture.ts --seconds=30 # 파일(TS) 30초

# 백그라운드 송출 (SSH 끊어도 유지, 로그 즉시 기록)
nohup stdbuf -oL ./cg-streamer --run --project=... --udp=10.10.10.11:1234 > ../run_udp.log 2>&1 &

# 저작 (GUI 세션 필요: DISPLAY=:0)
DISPLAY=:0 XAUTHORITY=/home/pi/.Xauthority ./cg-streamer --edit --project=project.json

# HDMI 로 실제 송출 화면 확인 (UI+영상 합성 그대로, 인코딩/UDP 는 정상 유지)
DISPLAY=:0 XAUTHORITY=/home/pi/.Xauthority ./cg-streamer --run --preview --project=... --udp=10.10.10.11:1234

# UI 만 확인 (영상 없음, 인코딩 없음, --cef:use-angle=gles-egl 은 이 보드에서 EGL 초기화 실패 -> swiftshader 로 대체)
DISPLAY=:0 XAUTHORITY=/home/pi/.Xauthority ./cg-streamer --run --view \
  --cef:use-gl=angle --cef:use-angle=swiftshader-webgl --project=...
```

### 옵션
| 옵션 | 설명 |
|---|---|
| `--run` / `--edit` | 송출 / 저작 모드 |
| `--project=경로` | 프로젝트 JSON (기본 `project.json`) |
| `--udp=호스트:포트` | UDP 송출 (`pkt_size=1316`) |
| `--out=URL또는파일` | 출력 지정 (파일이면 `.ts`) |
| `--seconds=N` | N초 후 자동 종료 |
| `--paint-fps=N` | CEF 렌더링 fps (기본 60, 출력은 항상 60fps). 60 미만이면 지터 버퍼 대신 최신 그림 방식 |
| `--no-sync` | UI 지터 버퍼 끔 (지연 0, 대신 CEF 박자와 어긋나 애니메이션이 가끔 끊김) |
| `--gpu` | OnPaint 경로에서 GPU 합성 유지 (`--cef:use-angle=gles-egl --cef:ozone-platform=headless`와 함께). 실측상 CPU 모드보다 무거움 |
| `--view` | 인코딩 없이 전체화면 창으로 재생 (DISPLAY 필요). UI만 보이고 영상은 안 나옴(아래 "로컬 화면 미리보기" 참고) |
| `--preview` | 인코딩(UI+영상 합성)은 그대로 하면서 그 결과를 로컬 X11 창(DISPLAY 필요)에도 표시. `--udp`/UDP 송출과 동시 사용 가능 |
| `--no-encode` | 렌더링만 (CEF 부하 측정용) |
| `--accel` | 실험: OnAcceleratedPaint(dmabuf) — `rk3588-accel` 빌드 필요 |
| `--cef:이름[=값]` | 크로미움 스위치 추가 (예: `--cef:use-angle=gles`) |

### 실행 중 제어 (UDP 127.0.0.1:5555)
SSH 로 보드에 접속해서 `cgctl.sh` 로 제어한다 (보드 자신에게서 온 UDP 만 받음).
```bash
cd /root/work/CGServer
./cgctl.sh next       # 다음 페이지 (페이지의 video 설정에 따라 mp4/HDMI 소스도 전환)
./cgctl.sh prev       # 이전 페이지
./cgctl.sh goto 2     # N번째 페이지 (1부터)
./cgctl.sh quit       # 종료
./cgctl.sh status     # 실행 여부 + 최근 로그/[stat]
```
다른 PC에서: `ssh pi@<보드IP> /root/work/CGServer/cgctl.sh next`

직접 보내기:
```bash
echo next   | nc -u -w0 127.0.0.1 5555    # 다음 페이지
echo prev   | nc -u -w0 127.0.0.1 5555    # 이전 페이지
echo goto 2 | nc -u -w0 127.0.0.1 5555    # N번째 페이지 (1부터)
echo quit   | nc -u -w0 127.0.0.1 5555    # 종료
```
시청: `ffplay -fflags nobuffer udp://@:1234` (받는 쪽에서 UDP 1234 수신 허용)

### 로그
```
[cg] run: project=... out=... paint=software paint_fps=60 sync=jitter-buffer(3)
[video] play /root/work/CGServer/web/girsday.mp4 (h264, 23.976fps)
[stat] enc=60.0fps paint=60/s out=8.03Mbps rga=3.3ms video=on uiq=2 under=0 drop=0 accel_fail=0
```
| 항목 | 의미 |
|---|---|
| `enc` | 인코딩 fps (60이어야 정상) |
| `paint` | 초당 CEF OnPaint 수 (정지 화면이면 0 — 정상, 직전 그림을 반복 인코딩) |
| `out` | 출력 비트레이트 (정지/단색 화면은 8Mbps 미만으로 떨어짐) |
| `rga` | 프레임당 합성(RGA) 평균 시간 — 16.7ms 를 넘으면 60fps 불가 |
| `video` | 영상 합성 상태 (`on`=mp4, `hdmirx`, `hdmirx(no-signal)`, `off`) |
| `uiq` / `under` / `drop` | UI 지터 버퍼 길이 / 애니메이션 중 늦어서 반복한 누적 횟수 / 넘쳐서 버린 누적 수 |

## 영상 합성 (HTML UI + 네이티브 MPP 디코딩)
공식 CEF는 H.264 `<video>`를 재생하지 못하므로(특허 코덱·V4L2 미포함), 영상은 앱이 직접 MPP로 디코딩해
HTML UI 아래에 합성한다. libcef 재빌드 불필요.
```
[UI]   player.html (배경 투명) → CEF OnPaint(BGRA, 프리멀티플라이드 알파) → UI 변경 시만 dmabuf 업로드
[영상] mp4 → libavformat → MPP 디코딩(NV12 dmabuf) ─┐
       RGA: 영상→캔버스(비율 유지) → UI 알파 합성 → NV12 ─┴→ MPP H.264 → TS/UDP
```
프로젝트 JSON (페이지별):
```json
{ "bg": "transparent",
  "pages": [ { "html": "...", "css": "...",
               "video": "/root/work/CGServer/web/girsday.mp4",
               "videoRect": [60, 110, 1280, 720] } ] }
```
- `video`: 파일 경로(상대경로면 프로젝트 파일 기준). H.264/H.265/VP9/VP8, 끝나면 반복 재생.
  `"hdmirx"`(또는 `"/dev/videoN"`)이면 HDMI 입력을 라이브로 합성 (아래 "HDMI 입력").
  페이지마다 다르게 줄 수 있고, 페이지 전환(next/prev/goto) 시 소스가 자동으로 바뀐다(한 번에 1개).
- `videoRect`: 영상을 맞춰 넣을 영역(비율 유지, 생략 시 전체화면). 페이지에 `video`가 없으면 정지.
- `bg`를 `transparent`로 해야 영상이 보인다. 영상 자리만 투명하게 두고 나머지를 칠하려면
  예: `.hole{left:60px;top:110px;width:1280px;height:720px;box-shadow:0 0 0 4000px #0b1d3a}`
- 이미지(`<img src="file:///...">`)·반투명 요소도 그대로 합성된다. 페이지 안 `<script>`는 실행되지 않음
  (player가 innerHTML로 삽입) — 애니메이션은 CSS로.
- cefQuery 직접 제어: `video:play:<경로>`, `video:stop`, `video:rect:x,y,w,h`
- 예제: `build/video_project.json`(전체화면 영상), `build/image_project.json`(영상 축소 + 이미지 패널, `build/assets/`)
- 합성은 RGA 2단계: 영상 → BGRA 캔버스(영역이 바뀔 때만 검정 채움), `imcomposite`(UI over 캔버스 → 인코더 NV12).
- 측정(1080p H.264 + 자막/티커, 1080p60 출력): 전체화면 영상 약 16%, 이미지 패널 구성 약 14% (top 기준).
- 예제: `build/two_page_project.json` — 1페이지 mp4, 2페이지 HDMI 입력.

### HDMI 입력 (라이브 소스)
CM3588 의 HDMI IN(`/dev/video20`, rk_hdmirx)을 영상 자리에 합성한다.
```json
{ "bg": "transparent", "pages": [ { "html": "...", "css": "...", "video": "hdmirx", "videoRect": [60, 110, 1280, 720] } ] }
```
- V4L2 버퍼를 dmabuf 로 내보내 RGA 가 복사 없이 읽음. 입력 포맷 BGR3/NV12/NV16/NV24 자동 처리, 4K 는 축소.
- 신호 없음이면 영상 자리는 검정(UI 는 그대로), 신호가 들어오거나 해상도가 바뀌면 자동 재연결.
- 신호 확인: `v4l2-ctl -d /dev/video20 --query-dv-timings`
- 예제: `build/hdmirx_project.json`. 측정(4K60 NV12 입력 → 1280x720 영역): enc 60fps, rga 약 8.5ms/프레임, 전체 CPU 약 15%.
- 음성은 미지원. 색공간 변환은 RGA 기본값(BT.601) — HD/4K 소스(BT.709)는 색이 약간 다를 수 있음.

## 로컬 화면 미리보기 (--preview / --view)
`--run`(기본)은 CEF 를 오프스크린(windowless)으로 렌더링해 화면에 아무 창도 띄우지 않는다(인코딩 전용).
로컬 HDMI 로 확인하려면 두 모드 중 하나를 쓴다:

- **`--preview`** (권장): 인코딩 파이프라인(UI+영상 RGA 합성 → NV12)은 정상 동작시키면서, 매 프레임 그
  결과를 RGA 로 축소+BGRX 변환해 별도의 평범한 X11 창(override-redirect, GL/EGL 미사용)에 `XPutImage` 로
  그린다. UDP 송출과 동시에 켤 수 있고, 실제 송출 화면과 100% 동일한 내용이 보인다. 미리보기는 20fps 로
  스로틀(X11 소켓 부하 절감), 인코딩 자체는 60fps 그대로.
  - 구현: `mpp_encoder.*`의 `ExportPreviewBgrx()`(마지막 `Encode()`가 쓴 NV12 버퍼를 그대로 재사용, 스레드
    안전은 EncodeLoop 스레드 내 동기 호출에 의존) + `main.cpp`의 `PreviewLoop()`.
- **`--view`**: CEF 의 실제 Views 창(GPU 필요)으로 player.html 을 띄운다. **이 보드(CM3588, Mali G610 +
  libmali-valhall-g13p0)에서 `--cef:use-angle=gles-egl` 은 GPU 프로세스가
  `No suitable EGL configs found for initialization` 로 실패**해 창이 제대로 안 뜬다(397x51 같은 비정상
  크기). `--cef:use-angle=swiftshader-webgl`(소프트웨어 GL)로 우회하면 전체화면 창은 정상적으로 뜬다.
  다만 이 모드는 인코딩을 끄기 때문에(`g_view`→`g_encode=false`), 영상 합성(RGA)이 붙는
  `EncodeLoop`을 아예 안 타서 **UI(자막·로고·패널)만 보이고 mp4/HDMI 영상은 비어 있다** — 레이아웃/자막
  확인용으로만 쓸 것.

## 알아둘 점
- 실기 확인(CM3588): 1080p60 송출, 색상(BGRA) 정상, UDP 페이지 제어 정상, 영상 합성 정상, `--preview` 로컬 미리보기 정상(UDP 송출과 동시).
- **UI 지터 버퍼**: CEF와 인코더의 60Hz 박자 차이로 생기는 끊김(같은 그림 2번/1장 건너뜀)을 없애기 위해
  UI 그림을 3장 쌓아 출력 → UI 지연 약 50ms. CEF 자체가 부하로 가끔 1프레임을 건너뛰면(약 15~20초에 1회)
  그 순간의 걸림은 남는다(`under` 증가). 애니메이션은 `left` 대신 `transform`, 큰 `box-shadow` 대신 div 를 쓰면 줄어든다.
- **editor.html 과 영상 설정**: 편집기는 GrapesJS 전용 `data` 가 있는 페이지만 불러오고, 저장 시 `video`/`videoRect` 를
  보존하지 않는다 → 영상 합성 프로젝트는 아직 JSON 을 직접 작성해야 함(편집기로 덮어쓰지 말 것).
- 화면을 많이 덮는 불투명 배경/그림자는 CEF 소프트웨어 합성(VizCompositor) 부하를 키운다.
- CEF 125 미만이면 main.cpp 의 `runtime_style` 줄 삭제.
- 알파 출력(Fill+Key)은 미구현.

## 실험: dmabuf 제로카피 경로 (--accel) — 이 보드에서는 동작하지 않음
```bash
cmake --preset rk3588-accel && cmake --build --preset rk3588-accel
cd build/rk3588-accel/Release
./cg-streamer --run --accel --cef:use-angle=gles-egl --cef:ozone-platform=headless --project=...
```
- 흐름: CEF(GPU 합성) → OnAcceleratedPaint(dmabuf) → RGA(fd) → NV12 → MPP → TS/UDP
- 실측: GPU 프로세스는 Mali로 동작하지만 **OnAcceleratedPaint 콜백 0회** (CEF Linux 공유 텍스처 미검증 경로,
  cef#3687). 자세한 내용은 ../history.md.
