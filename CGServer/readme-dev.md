# 개발환경 (Debian 12 / RK3588)

테스트·빌드 단말은 FriendlyElec CM3588(RK3588)이며 OS 는 **Debian 12 (bookworm) arm64** 이다.
(이전 Debian 11 환경은 `readme-env-d11.md`, `readme-history-d11.md` 참고)

## 단말 정보
| 항목 | 값 |
|---|---|
| 접속 | `ssh pi@10.10.10.56` (포트 22, 비밀번호는 `readme-test.txt`) |
| 사양 | 8코어, 메모리 약 3.9GB, aarch64 |
| 작업 경로 | `/home/pi/work/github.cgserver/CGServer` (GitHub clone) |
| 최종 소스 백업 | `/userdata/github.cgserver_YYYYMMDD.tgz` (CEF 런타임·미디어 포함, .git 제외) |
| 백업 풀어 둔 곳 | `/home/pi/work/final/CGServer` (참고용) |

- `sudo` 사용 가능. 사용자 `pi` 는 `video`, `render`, `input` 그룹(`/dev/mpp_service`, `/dev/rga`, `/dev/dma_heap/*` 접근).
- 단말 호스트 키는 OS 재설치로 바뀌었으므로 PC 의 `known_hosts` 를 갱신했다.

## 소스 구성
1. `git clone https://github.com/lesc0/HutonCGServer.git /home/pi/work/github.cgserver`
2. git 에 없는 런타임 파일(libcef.so, media, locales 등)은 `/userdata` 최종 소스에서 `bin/` 으로 복사:
   ```bash
   mkdir -p /home/pi/work/final && tar xzf /userdata/github.cgserver_YYYYMMDD.tgz -C /home/pi/work/final
   cp -an /home/pi/work/final/CGServer/bin/. /home/pi/work/github.cgserver/CGServer/bin/
   ```
3. `lib/`(ffmpeg/mpp/rga/x11 벤더 라이브러리)는 Debian 11 용이라 **Debian 12 빌드에서는 쓰지 않는다**(시스템 패키지 사용).

## 빌드 도구 (apt)
```bash
sudo apt-get install -y build-essential cmake ninja-build pkg-config git \
  libx11-dev libxext-dev libxrandr-dev libgtk-3-dev libnss3 libx11-xcb1 libxcomposite1 \
  libxdamage1 libgbm1 libdrm-dev libasound2-dev libcups2 libxss1 \
  libswscale-dev libswresample-dev libavformat-dev libavcodec-dev libavutil-dev \
  librockchip-mpp-dev librga-dev
```
| 도구 | 버전 |
|---|---|
| gcc/g++ | 12.2 |
| cmake | 3.25.1 (프리셋 요구 3.21 이상 충족, 별도 설치 불필요) |
| ninja | 1.11.1 |
| node / npm | **22.23.3 / 10.9.9** (cg-editor 가 `>=22.13` 요구 → apt 의 18 대신 아래 "Node.js" 로 설치) |
| ffmpeg dev | 5.1.6 (rockchip 빌드, `libavcodec.so.59`) |
| rockchip-mpp / librga | 20260226-2 / 2.2.0-1 (시스템 패키지) |

## Node.js (cg-editor 용, 공식 바이너리)
cg-editor(Next.js 16)는 Node `>=22.13` 이 필요해 Debian 12 의 apt 패키지(18.x)는 쓰지 않는다(`nodejs`, `npm` apt 패키지는 제거).
```bash
# https://nodejs.org/dist/latest-v22.x/ 의 linux-arm64 (SHASUMS256.txt 로 검증)
sudo mkdir -p /opt/node && sudo tar xJf node-v22.23.3-linux-arm64.tar.xz -C /opt/node
for b in node npm npx; do sudo ln -sfn /opt/node/node-v22.23.3-linux-arm64/bin/$b /usr/local/bin/$b; done
node -v   # v22.23.3 , npm 10.9.9
```
- 설치 경로 `/opt/node/node-v22.23.3-linux-arm64`, 실행 링크 `/usr/local/bin/{node,npm,npx}`.
- 이후 `src/cg-editor/build.sh` 가 `npm ci` 후 빌드한다(인터넷 필요).

## CEF (빌드하지 않고 최종 소스의 libcef.so 를 링크)
- 위치: `/opt/cef/cef_custom_130_arm64` , 환경변수 `CEF_ROOT=/opt/cef/cef_custom_130_arm64` (`~/.bashrc` 에 등록)
- 원본: 저장소 `doc/cef/cef_custom_130_arm64.tar.gz` (git 제외, PC 에만 있음). Chromium 130 패치 커스텀 빌드의 `Release/`(libcef.so 등), `Resources/`, `include/`, `libcef_dll/` 소스만 들어 있다.
- 이 tar 에 **없어서 보충한 것** (같은 버전 공식 배포본 `cef_binary_130.1.16+g5a7e5ed+chromium-130.0.6723.117_linuxarm64_minimal` 에서 가져옴):
  - `cmake/`, `CMakeLists.txt`, `libcef_dll/CMakeLists.txt` (FindCEF.cmake 등)
  - `include/` 전체 (`cef_version.h`, `cef_config.h`, 생성된 `base/internal/cef_net_error_list.h` 등 — 커스텀 tar 의 include 는 소스 트리용이라 공식 것으로 교체)
  - 공식 배포본의 바이너리는 사용하지 않는다.
- 최종 소스(`bin/`)에서 복사해 둔 런타임 파일: `Release/chrome-sandbox`, `Release/libvulkan.so.1`, `Resources/{chrome_100_percent,chrome_200_percent,resources}.pak`, `icudtl.dat`, `locales/`
- 공식 배포본 다운로드 시 HTTP/2 로 끊기면 `curl --http1.1 -C -` 로 이어받는다.

## 빌드
```bash
cd /home/pi/work/github.cgserver/CGServer/src/cg-streamer
./build.sh              # release → build/rk3588-release/out
./build.sh install      # release 빌드 후 CGServer/bin/ 으로 복사
./build.sh debug | clean
./build.sh --jobs 4     # 동시 컴파일 수 (기본 2, 메모리 3.9GB 라 OOM 주의)
```
- 컴파일·링크 확인 완료 (`ldd` 누락 라이브러리 없음). 실행 중인 `cg-streamer` 는 재시작해야 새 실행 파일이 반영된다.
- Debian 12 대응 소스 수정: `video_source.cpp` 에 `#include <libavcodec/bsf.h>` 추가 (ffmpeg 5.x 는 `avcodec.h` 에 bsf 선언이 없음).
- 빌드 로그 예: `/home/pi/work/build.log`

## 실행
`CGServer/bin/start.sh` (rebuild.sh / stop.sh 참고). 절차는 `readme-test.txt`.
```bash
cd /home/pi/work/github.cgserver/CGServer/src/cg-editor && ./build.sh   # 최초 1회(npm ci + Next 빌드)
cd ../../bin && DISPLAY=:0 ./start.sh      # cg-editor(8080) + cg-streamer + 키오스크 (CG_SKIP_KIOSK=1 이면 키오스크 생략)
```
- 실시간 우선순위: `/etc/security/limits.d/99-cg-rt.conf` 에 `pi - rtprio 90` 설정함(새 로그인부터 적용). 엔진이 쓰려면 `bin/cgsetup.cfg` 에 `realtime=on`(현재 off).
- 원격(ssh)에서 실행할 때 명령 줄에 `cg-streamer --run` 문자열이 들어 있으면 start.sh 가 이미 실행 중으로 오인한다.

## 기타 환경 정보
- **미리 설치돼 있던 시스템 패키지** (OS 이미지 기본): rockchip 빌드 ffmpeg 5.1.6, librockchip-mpp 20260226-2, librga 2.2.0-1, libv4l-rkmpp, gstreamer1.0-rockchip1, `chromium-browser-stable 143.0.7499.40`(`/opt/chromium.org/stable/chromium-browser`, 키오스크용). 이 패키지들은 apt 로 다시 설치하지 않는다.
- **화면**: lightdm + Xorg(:0) + xfwm4 데스크톱이 떠 있다. HDMI-1 = 에디터 화면(1024x600), HDMI-2 = 송출 모니터(미연결이면 `disconnected` 로 보이고 출력 없음). 연결 상태는 `/sys/class/drm/card0-HDMI-A-{1,2}/status`, `xrandr` 로 확인.
- **송출 설정** `bin/cgsetup.cfg`: `output=3`(로컬 미리보기 포함), `editor_display=HDMI-1`, `output_display=HDMI-2`, `editor_kiosk=off`(on 이면 start.sh 가 Chromium 키오스크로 에디터를 띄움), `realtime=off`, `udp_ip/udp_port`, `fps=60`, `editor_port=8080`.
- **cg-editor**: `src/cg-editor/build.sh` 결과는 `src/cg-editor/.next/` 와 `node_modules/`(둘 다 git 제외), 운영 모드 포트 8080. 빌드 결과·로그는 `build/rk3588-release/`, `bin/log/`.
- **재부팅**: 자동 시작 설정은 없다. 재부팅하면 `DISPLAY=:0 bin/start.sh` 를 다시 실행해야 한다(`ulimit -r`=90 은 유지됨).
- **PC 에서 단말 접속**: ssh 비밀번호 로그인(키 등록 없음). 단말 계정 `pi` 는 `sudo` 가능(비밀번호는 `readme-test.txt`).
- **참고(`/home/pi/work`)**: `github.cgserver`(소스), `final`(최종 소스 tgz 풀어 둔 것, 참고용), 기존 `install_trzsz.sh`.

## PC(Windows) 쪽
- 작업 폴더 `D:\zPrj26-Huton-MediaServer\github.HutonCGServer\CGServer`, 원격 `https://github.com/lesc0/HutonCGServer.git`
- 단말 반영: PC 에서 push → 단말에서 pull → `build.sh install` → `start.sh`
- 소스 백업은 CGServer 폴더 압축(node_modules, .git, build, .next 제외) 후 `/userdata` 에 보관.
