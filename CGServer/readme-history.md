# 작업 히스토리 (cef_mpp / zcgserver)

## 2026-09-29

### 프로젝트 통합

- `C:\Users\ssang\Downloads\files`에 있던 두 예제(최상위 느슨한 파일 세트 `main.cpp`/
  `mpp_encoder.*`/`CMakeLists.txt`/`README.md`와, `cef_mpp.zip`을 풀어놓은
  `cef_mpp/` 폴더)를 diff로 비교 → 소스는 완전히 동일하고, `cef_mpp/` 쪽만
  `ts_muxer.*`와 저작/재생용 `web/`(editor.html, player.html, GrapesJS)을
  추가로 포함한 완전한 버전임을 확인.
- 완전한 `cef_mpp/` 트리를 이 저장소의 `zcgserver/`로 복사해 단일 프로젝트로
  구성 (이후 사용자가 `test_cef_mpp/` 하위 폴더로 재배치, `.vscode/settings.json`의
  `cmake.sourceDirectory`가 이를 가리킴).

### main.cpp 모드 구분 리팩터

- 흩어져 있던 `bool g_run_mode` / `bool g_accel` 전역 플래그를
  `enum class AppMode { kEdit, kRun }`, `enum class PaintMode { kSoftware, kAccel }`로
  교체 ([main.cpp:46-53](test_cef_mpp/main.cpp#L46-L53)).
- `OnPaint`(소프트웨어, [main.cpp:111-124](test_cef_mpp/main.cpp#L111-L124)), `OnAcceleratedPaint`
  (dmabuf 실험, `#ifdef CG_ACCEL`, [main.cpp:126-164](test_cef_mpp/main.cpp#L126-L164)),
  `main()`의 edit/run 분기([main.cpp:420-456](test_cef_mpp/main.cpp#L420-L456))에
  `// ===== [MODE] =====` 주석 블록을 달아 어느 코드가 어느 모드에 속하는지 명시.
- 런타임 로그(`[cg] run: ... paint=software|accel`)에도 현재 PaintMode 표기 추가.

### 빌드 환경 문서화 (env.md) + RK3588 CMake 프리셋

- `test_cef_mpp/env.md` 신규 작성: 대상 보드(RK3588, Debian 12 arm64,
  **네이티브 빌드** — 크로스컴파일 아님), 필수 apt 패키지, CEF linuxarm64
  바이너리 배치 방법과 `CEF_ROOT` 환경변수 관례, 빌드/실행 명령, 실기 미검증
  제약 사항을 정리.
- `test_cef_mpp/CMakePresets.json` 신규 작성: VSCode CMake Tools에서 바로 고를 수
  있는 두 구성 제공.
  - `rk3588` — 기본 소프트웨어 `OnPaint` 경로 (`build/rk3588/`)
  - `rk3588-accel` — `ENABLE_ACCEL_PAINT=ON`을 자동 설정하는 `--accel` dmabuf
    실험 경로 (`build/rk3588-accel/`), `rk3588`을 `inherits`
  - 둘 다 `CEF_ROOT`는 하드코딩하지 않고 `$env{CEF_ROOT}`를 읽도록 함
  - `condition: hostSystemName == Linux`로 걸어 Windows에서 실수로 구성 못 하게 함

### OnAcceleratedPaint 호출 가능성 검토 (미해결 — 실기 필요)

`--accel`(dmabuf 제로카피, [main.cpp:130](test_cef_mpp/main.cpp#L130))이 실제로 호출되려면
3단계가 전부 맞아야 함을 확인:

1. 컴파일 타임: `rk3588-accel` 프리셋(`CG_ACCEL` 정의)으로 빌드해야 함수 자체가 포함됨.
2. 런타임: `--accel` 플래그로 `PaintMode::kAccel`이 되어야
   `wi.shared_texture_enabled = true`([main.cpp:429](test_cef_mpp/main.cpp#L429))가 걸리고
   GPU가 꺼지지 않음([main.cpp:242](test_cef_mpp/main.cpp#L242)).
3. **미검증**: CEF의 GPU 프로세스가 RK3588 Mali 드라이버 위에서 실제로
   OSR용 공유 텍스처(dmabuf)를 만들어내야 콜백이 옴. Mali는 기본적으로 AFBC
   압축 버퍼를 만드는 경우가 많은데, 코드는 `modifier`가 linear가 아니면
   해당 프레임을 그냥 버림([main.cpp:147-158](test_cef_mpp/main.cpp#L147-L158)) → AFBC를
   못 끄면 이 경로가 계속 `accel_fail`만 쌓일 가능성.

결론: 소프트웨어 경로(`rk3588`)는 그대로 동작할 가능성이 높으나, `--accel`은
설계상 가능한 구조일 뿐 보드에서 직접 돌려 `[accel] planes=...` 로그가
찍히는지 확인하기 전까지는 실제 호출 여부를 알 수 없음(README의 "콜백이
전혀 안 오면 `chrome://gpu` 확인" 안내와 일치). **다음 실기 검증 시 이 로그부터
확인할 것.**

### CEF 공식 문서/이슈 조사 — ARM/Mali 선례 없음, Linux 자체가 미검증

웹 검색으로 확인한 사실 (자세한 내용은 [test_cef_mpp/env.md](test_cef_mpp/env.md)의
"`--accel` 실기 검증 체크리스트" 절 참고):

- CEF 공식 ARM64 빌드 문서(BranchesAndBuilding, `tools/gn_args.py`)는 sysroot/GN
  인자 등 **컴파일 절차**만 다루고, Mali GPU나 dmabuf/AFBC 관련 내용은 없음.
- Windows는 D3D11 기반 shared texture가 CEF 71부터 **공식 지원**되지만, Linux는
  CEF 팀이 이슈 트래커에서 직접 "존재는 하지만 cefclient 참고 구현이 없어 검증된 적
  없다"고 밝힌 상태([cef#3687](https://github.com/chromiumembedded/cef/issues/3687)) —
  즉 `shared_texture_enabled`를 켜도 Linux에서는 조용히 `OnPaint`로 폴백될 수 있음.
- ARM64로 포팅된 참고 구현은 없음. 가장 가까운 예시인 java-cef PR #524도 x86 Linux
  기준이고 RK3588/Mali 검증 사례는 전무.
- NVIDIA proprietary 드라이버에서도 GBM/dmabuf 관련 버그가 최근까지 보고됨
  ([cef#4237](https://github.com/chromiumembedded/cef/issues/4237)) — x86+주요 벤더
  조합도 아직 불안정, RK3588+Mali는 더더욱 선례 없는 조합.
- Chromium 자체는 99버전부터 `--use-angle=gl-egl --headless`로 헤드리스 GPU 렌더링을
  지원하고, RK3588의 오픈소스 Mali 드라이버 Panfrost도 대체로 가속이 되는 편으로
  알려져 있음 — 이 레이어(OpenGL/EGL 자체가 되는지)는 CEF와 별개로 먼저 확인 가능.

이에 따라 env.md에 실기에서 어디를 봐야 하는지(①GPU 드라이버 → ②CEF GPU 프로세스
→ ③앱 자체 `[accel]`/`accel_fail` 로그) 3단계 체크리스트를 추가함.

### Armbian 포럼 실사용 사례 (Orange Pi 5 / RK3588) 반영

사용자가 제공한 [armbian forum 글](https://forum.armbian.com/topic/26188-hardware-acceleration-with-chromium)을
조사해 env.md의 필수 패키지/체크리스트에 반영:

- 실제로 RK3588에서 Chromium GPU 가속(4K 유튜브 CPU 10~15%까지 낮춘 성공 사례 등)을
  이룬 보고가 존재함 — 단 바닐라 Debian/Ubuntu 저장소 Mesa/Panfrost가 아니라
  `mali-g610-firmware` + RK3588 전용 패치가 들어간 Panfrost 포크("panfork-mesa",
  `ppa:liujianfeng1994/panfork-mesa`)를 썼을 때 얘기임.
- **Wayland 세션이 사실상 필수**였다는 보고 다수 — X11에서는 문제 발생, Wayland로
  전환 후 해결. 우리 앱은 디스플레이 서버 없는 완전 헤드리스를 가정했는데, GPU 합성이
  Wayland 컴포지터 존재 자체에 의존할 가능성이 새로 제기됨 — 헤드리스 Weston 같은
  걸 같이 띄워야 할 수도 있다는 뜻이라 실기 검증 항목에 추가.
- `apt upgrade`가 GPU 드라이버를 깨뜨린 사례, `libva`(VAAPI)는 RK3588 미지원(우리는
  MPP/RGA를 직접 쓰므로 무관) 등도 확인.
- env.md "필수 패키지"와 "`--accel` 실기 검증 체크리스트" 1단계에 위 내용을 반영함.

### Armbian 포럼 추가 조사 — 커널별 드라이버 갈림길 (중요)

같은 포럼에서 관련 스레드를 더 찾아봄:

- **["Expected default graphics acceleration for RK3588?" (2025-11~12)](https://forum.armbian.com/topic/56374-expected-default-graphics-acceleration-for-rk3588/)**
  — Mali-G610은 커널에 따라 배타적인 두 경로로 갈림:
  - 벤더 6.1 BSP 커널 + `libmali`(proprietary): NPU 지원, GPU는 "기본" 수준
  - 메인라인 6.12~6.18 커널 + `Panthor`(오픈소스): GPU 완전 가속, NPU 미지원
  - 우리는 NPU 불필요 → 목표는 Panthor+메인라인 6.12+ 커널이지만, 보드 벤더 이미지는
    보통 6.1 BSP인 경우가 많아 `uname -r` 확인이 먼저 필요.
  - **Armbian 관리자 Igor**: "(이 가속 경로는) 6.1 + 데스크톱(Gnome) 빌드에서만
    작동 확인됐다"는 취지로 언급 — 헤드리스 서버 구성 자체가 커뮤니티에서 검증 안
    된 조합일 가능성을 시사.
- **["How to setup libwc--or any alternative wayland compositors?"](https://forum.armbian.com/topic/29604-how-to-setup-libwc-or-any-alternative-wayland-compositors/)**
  — 디스플레이 없는 헤드리스 Wayland 컴포지터(`weston --backend=headless` 등) 구성의
  구체적인 성공 사례를 찾지 못함. 주로 패키지 의존성 빌드 에러 해결에 그침 —
  "헤드리스+GPU가속" 조합은 커뮤니티에서도 미해결/미검증 영역으로 보임.

즉 지난번 파악한 "Wayland가 필요할 수 있다"는 사실에서 한 걸음 더 나아가, **어떤
커널을 쓰느냐(벤더 6.1 vs 메인라인 6.12+)에 따라 드라이버 자체가 바뀌고, 게다가
검증된 사례들은 전부 헤드리스가 아니라 데스크톱(Gnome) 환경**이라는 게 확인됨.
env.md 체크리스트 1단계에 커널 확인(`uname -r`)과 드라이버 경로 표, 이 위험
요소를 추가함.

### Radxa 공식 문서/포럼 조사 — 상대적으로 긍정적인 신호

Armbian(커뮤니티 빌드) 대신 **Radxa 공식 문서/포럼**([Switch GPU driver](https://docs.radxa.com/en/rock5/rock5c/radxa-os/mali-gpu),
[Hardware Acceleration in Chromium using Debian](https://forum.radxa.com/t/hardware-acceleration-in-chromium-using-debian/8216))
을 확인:

- Debian 12(Bookworm) 기반 **Radxa OS는 Panthor(오픈소스)가 기본값** — 벤더 BSP
  커널에도 Panthor 패치가 이미 백포트된 것으로 보임. 지난번 Armbian 글의 "6.1 벤더
  커널=libmali만 가능"이라는 전제가 보드/이미지에 따라 다를 수 있음을 시사.
- Mali 블롭 전환 시 패키지명이 `libmali-valhall-g610-g24p0-x11-wayland-gbm` 형태 —
  접미사에 **`gbm`**(디스플레이 서버 없는 오프스크린 렌더링 표준 인터페이스)이 포함된
  변형이 공식적으로 존재함. 우리 헤드리스 CEF 앱과 개념적으로 가장 가까운 조합이라
  주목할 만하나, 이 블롭의 정확한 OpenGL/Vulkan 지원 범위는 문서를 직접 열어 재확인 필요.
- **Panthor가 2024-07에 Radxa Rock 5B에서 OpenGL ES 3.1 conformance 통과**
  ([CNX Software](https://www.cnx-software.com/2024/07/18/panthor-open-source-driver-achieves-opengl-es-3-1-conformance-with-arm-mali-g610-gpu-rk3588-soc/))
  — 지금까지 찾은 실패/미성숙 사례(Armbian Orange Pi 5 2023, Radxa ROCK 4 2021~2022)는
  전부 이 conformance 이전 시점 글이라, 현재(2026) 기준으로는 실제 상황이 그때보다
  나을 가능성이 있음. 다만 "헤드리스에서 검증됐다"는 사례는 Radxa 쪽에서도 못 찾음 —
  이 부분은 여전히 실기에서 직접 확인해야 함.

env.md의 같은 체크리스트 절에 이 내용을 추가함.

### Radxa / Orange Pi / FriendlyElec / Armbian 종합 조사 + CEF 자체 버그 (중요 — 기존 결론 일부 뒤집힘)

사용자 요청으로 3개 주요 RK3588 벤더(Radxa, Orange Pi, FriendlyElec)를 추가로
찾아보고, Armbian 커뮤니티의 가장 최신 글까지 확인함. env.md의
"`--accel` 실기 검증 체크리스트"를 벤더별 표 형태로 재구성함. 핵심 변경/추가 사항:

- **CEF와 Mali GPU를 직접 연결한 자료는 어디에도 없음.** Radxa/Orange Pi/FriendlyElec/
  Armbian 전부 "브라우저 Chromium"(유튜브 재생 등) 얘기만 하고 CEF의 OSR/
  `OnAcceleratedPaint`는 언급이 없음 — 이건 CEF 자체 이슈 트래커에서만 다뤄지는
  영역이고, 거기서도 RK3588/Mali 특정 사례는 없음.
- **새로 발견한 CEF 자체 버그**: [cef#2618](https://bitbucket.org/chromiumembedded/cef/issues/2618/onacceleratedpaint-is-not-called-with-off)
  — `--off-screen-rendering-enabled --shared-texture-enabled --external-begin-frame-enabled`를
  다 켜도 `OnAcceleratedPaint`가 호출되지 않는 사례가 보고된 적 있음. 하드웨어와
  무관한, CEF 버전 자체의 신뢰성 문제.
- **FriendlyElec**: Ubuntu 24.04 데스크톱 이미지는 Wayland+VPU/GPU 가속+Chromium을
  공식 통합해서 제공(구형 Xubuntu 이미지는 X11+Panfrost). "가속이 되는 이미지는
  Wayland 전제"라는 패턴이 FriendlyElec에서도 반복 확인됨.
- **`mali_kbase` 커널 모듈 충돌**: 벤더 커널의 `mali_kbase` 모듈이 로드돼 있으면
  GPU를 선점해서 Panthor/Panfrost가 아예 바인딩을 못 함 — 실기에서
  `lsmod | grep mali_kbase`로 확인 필요한 항목으로 체크리스트에 추가.
- **"Panthor는 Wayland 미지원"이라는 상반된 보고**도 있었는데, 이는 우리 코드가 이미
  대비 중인 AFBC(압축 버퍼) modifier 문제와 같은 종류로 추정됨 — Panthor가 만드는
  압축 버퍼를 컴포지터가 못 받아서 생기는 문제일 가능성.
- **⚠️ 가장 중요한 변경**: [Armbian "Setting Up Mali & HW Accel" (2025-05~11, 지금까지
  찾은 것 중 가장 최신)](https://forum.armbian.com/topic/51939-rk3588-setting-up-mali-hardware-acceleration/)에서
  관리자 Werner가 **"벤더 6.1.y 커널로 되돌리고 Panthor를 끈 뒤 proprietary
  `libmali`를 쓰라"**고 권고함 — 이전에 우리가 "목표"로 삼았던 "Panthor+메인라인
  커널이 정답"이라는 방향과 **정반대**. 즉 2024-07 conformance 소식만 보고 Panthor를
  최선으로 단정하면 안 되고, 실기에서는 Panthor와 libmali 두 경로를 **둘 다 시도해볼
  준비**를 해야 한다는 결론으로 수정함.

이 모든 내용을 env.md 체크리스트에 "벤더별 조사 요약" 표 + "알아둘 함정" 절로
재구성해 반영함.

## 2026-09-30

### 실기 첫 빌드/실행 (FriendlyElec CM3588 = RK3588)

- 보드 OS는 env.md 가정(Debian 12)과 달리 **Debian 11 (bullseye), glibc 2.31**.
  cmake 3.18 → 3.30.5를 `/opt/cmake-3.30.5-linux-aarch64`에 설치(`/usr/local/bin` 링크), ninja는 apt.
- CEF: 보드의 Chromium 130과 맞춰 `cef_binary_130.1.16+g5a7e5ed+chromium-130.0.6723.117_linuxarm64_minimal`
  을 `/opt/cef/`에 설치 (glibc 2.31에서 정상 동작).
- 빌드 디렉터리: `~/build/cef_mpp/{rk3588,rk3588-accel}` (소스 트리가 root 소유라 외부 빌드).
- CMakeLists.txt 수정 3건 (빌드가 안 되던 버그):
  1. `project(cef_mpp CXX)` → `C CXX` — CEF cmake가 C 컴파일러 플래그 검사를 함.
  2. `PROJECT_ARCH=arm64` 강제 — CEF는 `CMAKE_HOST_SYSTEM_PROCESSOR == "arm64"`만 검사해
     Linux 네이티브(`aarch64`)를 x86_64로 오인, `-m64 -march=x86-64`를 붙임.
  3. `libcef_lib`(libcef.so) 링크 추가 — wrapper만 링크해 `cef_string_*` undefined reference.
- **소프트웨어 경로(`--run`) 동작 확인**: enc≈60fps 유지, TS(H.264 High 1920x1080 60fps) 정상 디코드,
  색상 정상(BGRA 순서 OK), UDP `next` 페이지 전환 OK. 정적 화면은 paint=0/s이며 CBR 8Mbps 설정에도
  출력이 ~0.1Mbps로 떨어짐(MPP가 정적 프레임에 패딩 안 함).
- **`--accel` 경로: OnAcceleratedPaint 콜백 0회.**
  - 기본 옵션: `libGL error: failed to load driver: rockchip` → GPU 프로세스 초기화 실패로 종료.
  - `--cef:use-angle=gles --cef:use-gl=angle` 또는 `gles-egl + ozone-platform=headless`: GPU 프로세스는
    살아 있으나 `[accel]` 로그 없음 → cef#3687(Linux shared texture 미검증) 우려대로 콜백이 오지 않음.
  - 보드 GPU 스택: 벤더 커널 6.1 + `libmali-valhall-g610-g13p0-x11-gbm`, Mesa 20.3.5.

### <video> 태그 / MPP 하드웨어 디코드 연결 여부

- 공식 CEF 130(Spotify 빌드): WebM(VP9) `<video>` 재생 OK(소프트웨어 디코드, renderer CPU +~55%p),
  **H.264 mp4는 재생 불가**(검은 화면 — proprietary codec 미포함).
- libcef.so에는 V4L2 디코더 코드가 없음(`/dev/video-dec`, `libv4l2.so` 문자열 0개).
  VDA 관련 플래그(`--enable-accelerated-video-decode`, `CHROMIUM_USE_VDA=true` 등)를 줘도
  GPU 프로세스가 libv4l을 로드하지 않음 → **CEF `<video>`는 MPP로 연결되지 않음**.
- 보드의 FriendlyElec Chromium 130(`/opt/chromium.org`, 설정 `/etc/chromium.org/03-nanopi6`)은
  wrapper로 실행 시 GPU 프로세스가 `libv4l-rkmpp.so` + `libv4l2` 로드, `/dev/mpp_service` 오픈 확인
  → libv4l-rkmpp 경로 자체는 이 보드에서 동작. `/dev/video-dec0`은 libv4l-rkmpp용 설정 파일
  (`codecs=VP8:VP9:H.264:H.265`).
- CEF에서 쓰려면 같은 패치(V4L2 디코더 + libv4l 플러그인, proprietary codecs)를 적용해 CEF를 소스 빌드해야 함.

### 시스템 Chromium(MPP 디코딩) 소스 출처 조사

- 보드 Chromium: `chromium-browser-stable 130.0.6723.58` (`refs/branch-heads/6723@{#1353}`),
  바이너리에 "Rockchip Media Player" 문자열 → Rockchip(JeffyCN) 계열 패치 적용 빌드. FriendlyElec 자체 소스는 미확인.
- 동일 계열 130 패치 공개본: https://github.com/amazingfate/chromium-debian-build/tree/rockchip-rkmpp-130
  (Chromium 130.0.6723.116, `debian/patches/rkmpp/` 20개 — 0001 V4L2 video decoder HACK,
  0003/0004 libv4l2 stubs/plugins, 0016 Mali DDK X11 config, 0019 NV12 direct rendering 등)
  - gn args (debian/rules, arm64): `use_v4l2_codec=true use_vaapi=false use_v4lplugin=true`
    + `proprietary_codecs=true ffmpeg_branding="Chrome"`
- 원 패치 저장소: https://github.com/JeffyCN/meta-rockchip (dynamic-layers/recipes-browser/chromium,
  현재 master는 148~152 버전만 유지)
- 우리 CEF 130.1.16 = Chromium **130.0.6723.117** → 위 130 패치셋과 거의 같은 버전이라 CEF 소스 빌드 시
  chromium/src에 적용 가능성 높음(미검증).

### 영상 합성 경로 구현 (A 방식: HTML UI는 CEF, 영상은 네이티브 MPP) — libcef 재빌드 불필요

- 공식 CEF 130.1.16 그대로. `<video>` 대신 페이지 JSON의 `"video"`(+`"videoRect"`)로 앱이 직접 재생.
- 신규: `video_source.{h,cpp}` (libavformat 분리 + h264/hevc_mp4toannexb → MPP 디코딩, 원본 fps 페이싱, 반복 재생),
  `dma_heap_buf.h` (/dev/dma_heap/system-dma32 할당).
- `mpp_encoder`: `UploadUi()`(UI 변경 시만 dmabuf로 복사), `Compose()` — RGA 3단계(fd 기반):
  영상 NV12 → BGRA 캔버스(비율 유지) → UI 알파 합성(`SRC_OVER|PRE_MUL`) → NV12(인코더 입력).
  인코더 버퍼 그룹에 `MPP_BUFFER_FLAGS_DMA32` 추가.
- `main.cpp`: cefQuery `video:play:<경로>` / `video:stop` / `video:rect:x,y,w,h`, OSR 배경 투명
  (`background_color` ARGB 0), 인코딩 루프에서 영상 재생 중이면 합성 경로 사용.
- `player.html`: 페이지 `video` 있으면 재생 요청, 없으면 정지. `bg:"transparent"`가 `html`에도 적용되도록 수정.
- 결과 (girsday.mp4 1080p H.264 + HTML 자막/티커/LIVE, 1080p60 출력): enc 60fps, 합성 정상(반투명 포함),
  **전체 CPU 약 16%**(top 기준). 비교: 시스템 Chromium+ximagesrc 약 23%, 기존 CEF(720p VP9 SW 디코드) 약 33%.
  스레드: 브라우저 메인(OnPaint 복사) 17%, cg-encode(업로드+RGA+MPP) 16%, VizCompositor 15%, 렌더러 13+11%.

### 하단 티커 끊김 원인과 수정 (OnPaint 동기 인코딩)

- 증상: 티커가 매끄럽게 흐르지 않음. 출력 TS에서 티커 줄의 프레임 간 이동량 측정 결과
  (정상은 매 프레임 4px): **4px 114 / 0px 93 / 8px 92** (299프레임) — 성능이 아니라 **박자 문제**.
  CEF는 자체 60Hz로 매 프레임 그리는데(paint=60/s), 인코더는 별도 60Hz 타이머로 최신 그림을 가져가서
  두 시계가 어긋나 "같은 그림 2번 → 다음에 1장 건너뜀"이 반복됨.
- 시도 1: CEF 외부 BeginFrame(`external_begin_frame_enabled` + `SendExternalBeginFrame`)으로 인코딩 틱마다
  1장 요청 → CEF가 한 번에 1장만 처리해 렌더 지연(>16.7ms) 때 요청을 버림: paint 37~48/s, 오히려 악화 → 폐기.
- 적용: **OnPaint 도착에 맞춰 인코딩** (EncodeLoop가 condition_variable로 새 그림을 기다렸다가 인코딩,
  정지 화면이면 1.5주기 후 반복 인코딩). 결과 **4px 275 / 0px 12 / 8px 12** (92% 정상), enc 60fps.
  `--no-sync`로 기존 고정 타이머 방식, `--paint-fps`가 60 미만이면 자동으로 타이머 방식.
- (위 방식은 아래 지터 버퍼로 대체됨)
- 최종: **UI 지터 버퍼(2장)** — OnPaint 그림을 큐에 쌓고(최대 3장, 넘치면 가장 오래된 것 버림), 인코더는 고정 60Hz
  타이머로 2장 쌓인 뒤부터 매 틱 1장씩 꺼냄(1장만 있어도 2프레임 넘게 기다렸으면 출력 — 정지 화면 대응).
  추가 지연 약 33ms(최대 50ms). 결과 **4px 419/419 (100%)** ×2회, 정지 페이지 60fps 정상.
  `[stat]`에 `uiq`(큐 길이) `under`(비었던 횟수) `drop`(넘쳐 버린 수) 표시. `--no-sync`는 최신 그림 방식(지연 0).
  CEF와 인코더 시계의 미세한 차이로 장시간 운용 시 드물게 under/drop 1회(1프레임 반복/건너뜀)는 생길 수 있음.

### 지터 버퍼 3장 조정 + 남은 끊김 원인 (진행 중 메모)

- 조정: `kUiPrime=3`(지연 약 50ms), `kUiMax=5`, 큐가 비어도 다시 쌓일 때까지 기다리지 않음(1번만 반복),
  목표 수위(3) 초과가 1초 넘게 지속되면 1장 버려 지연 복귀. `under`는 "최근 2프레임 안에 그림이 왔는데 큐가 빔"
  (애니메이션 중 늦음)만 셈 → 정지 화면은 제외.
- 5.5분 무간섭 측정(image_project): **under 26 / drop 0**, enc 평균 60.010fps, **CEF paint 평균 59.927/s**
  (19,776장/330초), uiq 분포 0:7 1:83 2:232 3:10. (이전 2장 버퍼 6.5분: under 28 / drop 12)
- 해석: 빠진 그림 수(330초 × 0.083 ≈ 27)가 under(26)와 일치 → **CEF가 부하로 가끔 1프레임을 건너뜀**.
  CEF가 건너뛴 순간 애니메이션은 이미 2칸 진행돼 있으므로 우리 쪽 버퍼로는 없앨 수 없음
  (버퍼는 도착 간격 흔들림까지만 흡수 — 이번 조정으로 drop 0 달성).
- 대응(진행 중): 페이지 렌더링 부하 줄이기 — `build/image_project.json` CSS 수정
  (원본은 `build/image_project.before.json`에 보관)
  - 영상 둘레 남색 배경: `box-shadow: 0 0 0 4000px` → 배경 div 4개(위/왼쪽/오른쪽/아래)
  - 티커: `left` 애니메이션 → `transform: translateX` + `will-change: transform`
  - 썸네일 확대·이름 자막에 `will-change: transform` (별도 레이어)
  - 12초 녹화: 티커 4px 417/419.
  - 5.5분 측정: **under 20 / drop 1**, paint 평균 59.936/s, enc 60.010, uiq 0:4 1:90 2:217 3:19.
    **전체 CPU 약 20% → 약 14%**(60초 시점 top). 끊김은 26 → 20으로 조금 감소 — CEF가 여전히 약 0.06장/초 누락.
    VizCompositor 스레드가 여전히 가장 큼(약 53%, 1코어 기준) → 남은 누락은 CEF 소프트웨어 합성 부하로 추정.
- TODO: README/주석의 "2장(33ms)" 설명을 3장(50ms)으로 갱신.

### HDMI 입력(hdmirx) 라이브 소스 합성

- 하드웨어: CM3588 HDMI IN = `/dev/video20` (rk_hdmirx, V4L2 멀티플레인, 포맷 BGR3/NV24/NV16/NV12 — 입력 신호 따라 결정).
  시험 입력: 3840x2160 59.94fps NV12.
- 신규 `hdmirx_source.{h,cpp}`: QUERY/S_DV_TIMINGS → G_FMT → REQBUFS(MMAP 4) → EXPBUF(dmabuf) → STREAMON,
  poll 로 DQBUF 해 최신 버퍼 보관(이전 버퍼는 QBUF 반환), `V4L2_EVENT_SOURCE_CHANGE`/2초 무프레임 시 재연결,
  신호 없으면 0.5초마다 재시도. 페이지 `"video": "hdmirx"`로 사용. `VideoFrameRef.format`(RK_FORMAT_*) 추가.
- 첫 시험: 4K 입력에서 enc 53~57fps, uiq=5 drop 30 — RGA 4단계(검정 채움/축소/블렌드/NV12)가 16.7ms 초과.
- 개선: **RGA 2단계** — 캔버스에는 영상만(검정 채움은 영역 변경 시만),
  `imcomposite(UI, 캔버스, NV12 dst, SRC_OVER|PRE_MUL)`로 합성+변환 한 번에(이 보드 RGA에서 동작 확인,
  실패 시 blend+cvt 자동 대체). `[stat]`에 `rga=평균ms` 추가.
  결과: **4K HDMI 입력 enc 60fps, rga 8.5ms, drop 0, 전체 CPU 약 15%**. mp4 1080p 구성은 rga 3.3ms.
- 소스 전환 시험(`build/two_page_project.json`: 1p mp4, 2p hdmirx): next → hdmirx, prev → mp4 정상 전환,
  전환 중에도 enc 60fps.
- `cgctl.sh` 추가: SSH 에서 `./cgctl.sh next|prev|goto N|quit|status` (UDP 127.0.0.1:5555).
- 기존 TODO(README "2장(33ms)" → 3장(50ms)) 반영 완료.

### FFmpeg 라이브러리 출처

- `CMakeLists.txt`가 `pkg_check_modules(FFMPEG ... libavformat libavcodec libavutil)`로 링크하는 FFmpeg는
  별도로 받아오거나 소스빌드한 게 아니라 **apt로 설치된 시스템 패키지**(`libavformat-dev` 등).
- 버전: `7:4.3.4-1rockchip-r6-b230628` — 이름 그대로 **Rockchip이 커스터마이징한 빌드**이며,
  데비안 보안 저장소의 표준 `4.3.9`/`4.3.7`보다 우선순위(pin priority 100, 로컬/벤더 저장소)가 높게 잡혀 있어
  `apt install`만으로 이 버전이 선택됨. `ffmpeg`(CLI)도 같은 소스의 동일 버전.

### [cg-editor] 메인 메뉴가 바깥을 눌러도 닫히지 않는 문제
- 증상: 상단 메뉴(파일/편집/보기 …)를 열고 다른 곳을 눌러도 드롭다운이 계속 열려 있음.
- 원인: [app/page.tsx](src/cg-editor/app/page.tsx)의 메뉴는 메뉴 버튼 토글(`setMenu(menu===name?null:name)`)과 항목 선택 시에만 닫혔고, 바깥 클릭 처리가 없었음.
- 변경: `menu`가 열려 있는 동안 동작하는 `useEffect` 추가 (`openModal` 바로 위).
  - `pointerdown` / `mousedown` / `touchstart`(capture)에서 대상이 `.menugroup` 밖이면 `setMenu(null)`
  - `Escape` 키, 창 `blur` 시에도 닫힘
  - 실행 취소/다시 실행 기록(history) 로직은 변경하지 않음
- 확인: 개발 서버(5173)가 수정된 코드를 서빙하는 것과 VS Code 진단 오류 없음까지 확인.
- 미해결: 사용자가 "메뉴를 두 번 눌러야 없어진다"고 보고. 브라우저에서 직접 재현하지 못해 원인 미확정.
  - 확인 필요: 강력 새로고침(Ctrl+Shift+R) 후에도 동일한지, 바깥 클릭(a)인지 메뉴 버튼 재클릭(b)인지, 어느 영역(캔버스/패널/툴바)에서 발생하는지.

### [cg-editor] 참고
- `src/editor/client/src/TopBar.jsx`에도 별도의 상단 메뉴가 있으나(헤더에서 마우스가 벗어나면 닫힘) 이번에는 수정하지 않음.

## 2026-10-01

### 실행엔진 설계 + cg-streamer 구현 (cg-editor 결과물을 CEF 로 재생)

- 배경: cef-mpp 는 테스트용 소스, 실제 작업은 `src/cg-streamer/`. cg-editor(Next.js) 가 저장하는 프로젝트 JSON 을
  CEF 에서 그대로 재생하는 실행엔진을 만든다.
- 결정:
  - HTML 로 굽지 않고 **JSON 이 원본**, 공용 런타임이 JSON 을 읽어 렌더링 (글자 단위 렌더/효과 13종/시계/채널 상태머신이
    canvas+시간 함수라 HTML 변환 시 미리보기와 출력이 달라질 위험).
  - 영상 아이템의 효과는 실행엔진에서 지원하지 않음 (위치·크기·반복만 네이티브 MPP 로 전달).
  - 제어는 UDP → **HTTP** (UDP 는 전환기 병행). 응답/상태 조회 가능, 256바이트 제한 없음.
  - 폴더 규약: `bin/web/player.html`, `bin/web/cg-runtime.js`(공용), `bin/web/<프로젝트명>/project.json` + `media/`
    (src 는 상대경로, 공용은 `bin/web/_shared/`).
- 구현:
  - `bin/web/cg-runtime.js` + `player.html`: cg-editor 의 model/effects/text-render 를 Konva 없이 Canvas2D 로 포팅.
    Run Setting, Stamp 2채널, 효과 13종, 시계/계수기, 텍스트 갱신. 영상은 `cefQuery` 의 `video:*` 로 네이티브에 위임.
  - `src/cg-streamer/main.cpp`: HTTP 제어 서버(`--http/--bind/--token`, 기본 127.0.0.1:5555), 엔드포인트
    `/play /run /stop /clear /pause /cut /skip /next /prev /goto/N /stamp/{1|2}/{play|stop|pause} /global/{play|stop}`,
    `PUT /text/{linkName}`, `GET /status`, `POST /quit`. 명령은 CEF UI 스레드에서 `cg.cmd()` 로 실행, 인자는 화이트리스트 +
    JS 문자열 이스케이프. `cefQuery` 에 `base`(프로젝트 폴더 URL)·`state:`(상태 보고) 추가, `--autoplay` 추가.
  - `bin/cgctl.sh`: UDP(nc) → HTTP(curl) 로 변경.
  - 빌드: `CMakeLists.txt`/`CMakePresets.json` 을 `src/cg-streamer/` 로 이동, 실행파일 출력은 `bin/`
    (CEF 런타임 파일도 bin/ 으로 복사, 웹은 `bin/web/` 그대로 사용). `.gitignore` 는 `bin/*` 무시 + `bin/web/`·`bin/cgctl.sh` 예외,
    `doc/cef/*.tar.gz` 제외. `ZENV.md`/`cef_server_build.md` 빌드 명령 갱신.
- 검증: 헤드리스 Chrome 에서 샘플 프로젝트 렌더(한글 자막/외곽선/그림자/시계/wipe·fade·scale) 및 goto/text/clear 명령 확인.
  **C++ 변경은 Windows PC 에서 빌드하지 못해 컴파일 미검증** → 보드에서 빌드/송출 확인 필요.
- 제약/미구현: 에디터 "내보내기"(media/ 로 풀고 src 상대경로화) 없음 → `data:`·`/api/media` 미디어는 실행 불가,
  영상 동시 1개·Global 은 play/stop 만, 오디오 미재생, 폰트는 대상 머신에 설치 필요, clear/stop 은 Out 효과 없이 즉시.
- 기타: 2개 커밋에 섞여 GitHub 100MB 제한에 걸린 `doc/cef/cef_custom_130_arm64.tar.gz` 를 제외하고 커밋을 합쳐 푸시(`eff7b0c`).
- 문서: `doc/실행엔진설계.md`.

### [cg-editor] 실행 스크립트 추가 (start/stop/build, .env.production)
- `start.sh`·`stop.sh`·`build.sh` 및 Windows 용 `start.bat`·`stop.bat`·`build.bat` 추가 (src/cg-editor/).
  - `build`: Node 22.13+ 확인, node_modules 없으면 `npm ci`, `npm run build` (`--install`/`--check` 옵션).
  - `start`: 백그라운드 실행(PID/로그 `.run/`), 기본 개발 서버(5173), `--prod` 는 `dist/` 로 실행, `--port/--host/--fg` 옵션.
  - `stop`: PID 의 프로세스 그룹/트리 종료.
- `.env.production`: `CG_EDITOR_PORT=8080`(prod 모드 기본 포트, 옵션이 우선), `CG_EDITOR_HOST`(주석), `WRANGLER_SEND_METRICS=false`.
  `CLOUDFLARE_CF_FETCH_ENABLED` 는 기본값이 꺼짐이라 넣지 않음. cg-editor `.gitignore` 의 `.env*` 때문에 커밋되지 않음.
- 확인(Windows): `start.bat` 개발(5190)/`/prod`(8080) 기동 → HTTP 200, `stop.bat` 종료, `build.bat` 빌드 성공(`dist/`).
  배치 파일은 CRLF, `timeout` 대신 `ping` 으로 대기(콘솔 없는 환경에서 timeout 이 실패). `.sh` 는 문법 검사만(bash -n).

### [cg-editor] cg-editor3(doc/gptcode) 수정분 병합
- 내용: Stamp Playback(두 자막 채널, Top/Bottom 배치, 독립 재생·정지·반복·갱신), Global Animation Playback(배경 영상, On Air, 재생·일시정지·되감기, 구간 재생), 채널 설정·미디어 JSON 저장/복원.
- 방식: 덮어쓰기 없이 3-way 병합. 기준은 `CG-editor-source2.zip`, 내 쪽은 기존 src/cg-editor, 상대는 cg-editor3. 충돌 없음.
- 대상: app/channels.tsx(신규), editor-canvas.tsx, globals.css, model.ts, page.tsx, tests/editor-checks.ts, 다운로드-실행안내.txt.
- 확인: `tsc --noEmit` 통과. 화면 동작은 브라우저 확인 필요. 커밋/푸시는 아직 안 함.

### [cg-editor] cg-editor2(doc/gptcode) 수정분 병합
- 내용: Run Setting(페이지 구간·반복 횟수·대기·자동/수동), Playback(Clear·Cut·Skip·이전/다음), 효과 프리셋 수·Soft/Hard 보완.
- 방식: 덮어쓰기 없이 3-way 병합. 기준(base)은 `doc/gptcode/CG-editor-source.zip`, 내 쪽은 기존 src/cg-editor, 상대는 cg-editor2.
- 대상: app/attributes.tsx, editor-canvas.tsx, effects.ts, globals.css, model.ts, page.tsx, tests/editor-checks.ts, package-lock.json, 다운로드-실행안내.txt.
- 유지한 기존 변경: 패널 높이 조절(rowsplit), 메뉴 바깥 클릭 닫기, Effect 체크 해제.
- 확인: `tsc --noEmit` 통과. 화면 동작은 브라우저에서 확인 필요.
- 백업: 병합 전 app/, tests/를 세션 임시 폴더에 복사해 둠.

### [cg-editor] 미디어는 bin/media, 프로젝트는 bin/project (파일 서버 추가)
- 폴더: `bin/media/`(이미지·영상·음성 원본), `bin/project/`(프로젝트 JSON). `.gitignore`: media 내용은 제외(`.gitkeep`만), project 는 추적.
- 배경: 에디터 서버가 Cloudflare Worker 런타임(개발 vite+workerd, 프로덕션 wrangler)이라 디스크를 읽고 쓸 수 없음 →
  **파일 서버 `scripts/files-server.mjs`**(의존성 없는 Node, 기본 127.0.0.1:8081)를 추가하고 `start`/`stop` 이 에디터와 함께 기동/종료.
  - `GET /media[?kind=]` 목록, `GET /media/<파일>` 스트리밍(Range 지원), `GET /projects`, `GET|PUT /projects/<이름>`.
  - 안전장치: 파일명 검증(경로 구분자/`..` 거부), 확장자 화이트리스트, CORS 는 에디터와 같은 호스트만, 프로젝트 50MB 제한,
    저장은 임시 파일 후 교체, `cg-editor` 형식이 아니면 거부.
  - 프로젝트 저장 시 `data:` 미디어는 `bin/media/import-<해시>.<확장자>` 로 풀고 `src` 를 `../media/<파일>` 로 치환(내보내기 갭 해소).
- 에디터(`app/files.ts` 신규, `model.ts`, `page.tsx`, `channels.tsx`, `editor-canvas.tsx`, `audio-tracks.tsx`):
  - 미디어 src 는 `../media/<파일>` 로 저장(project 폴더 기준 상대경로 → cg-streamer 가 그대로 읽음), 화면에서는 `mediaUrl()` 로 파일 서버 주소로 변환.
  - 이미지·동영상·음성 버튼과 배경 영상(Global)은 PC 파일 선택 대신 **bin/media 목록 대화상자**에서 선택. 캔버스로 파일을 끌어다 놓으면 bin/media 에 같은 이름이 있을 때만 가져옴.
  - "보관함(D1/R2) 저장" → "bin/project 에 저장"으로 교체(목록·열기 포함). PC JSON 저장/열기는 유지.
- 설정: `src/cg-editor/.env`(공통: 포트 5173, 파일 서버 8081 등) 추가, `.env.production`(prod 8080)이 덮어씀, `--port/--host` 옵션이 최우선.
  (`.env*` 는 cg-editor `.gitignore` 로 커밋되지 않음)
- 검증: 파일 서버 단독 테스트(목록/Range/경로 탈출 차단/data: 풀기/CORS), `tsc --noEmit` 통과, Windows 에서 `start.bat`(dev 5173, prod 8080)·`stop.bat` 로
  에디터+파일 서버 동시 기동/종료 확인. **브라우저에서 실제 UI(미디어 선택·저장·열기) 조작은 미확인.**
- 제약: 파일 서버 포트는 `app/files.ts` 의 `FILES_PORT` 와 같아야 함. "PC에 JSON 저장"은 `../media/` 참조를 파일로 내장하지 않음.

### 실행 파일 이름 cef_mpp → cg-streamer
- CMake 타깃 `cg_streamer`, `OUTPUT_NAME cg-streamer` → 실행 파일 `bin/cg-streamer` (`project()` 이름도 `cg_streamer`).
- `bin/cgctl.sh`(`pgrep -x cg-streamer`), `main.cpp` 사용법 주석, `ZENV.md`, `cef_server_build.md`, `doc/실행엔진설계.md` 의 명령어를 새 이름으로 변경.
- 위의 이전 기록(2026-09-29 ~ 10-01)에 적힌 `cef_mpp` 는 당시 이름이라 그대로 둠. 이 PC(Windows)에서는 빌드하지 못해 보드에서 확인 필요.

### [cg-editor] 화면 정리: 타임라인을 Playback 패널 탭으로, 행 3개 → 2개
- 배경: 참고한 NABI HD(`doc/타사CG메뉴얼/NAVI.pptx`)는 위쪽(Page List / 캔버스 / Catalog)과 아래쪽(Attributes·Effects / Playback 계열 / Color) **2행**이고 타임라인 패널이 없음.
  cg-editor 는 타임라인 행이 하나 더 있어 화면이 꽉 찼음.
- 변경(`app/page.tsx`, `app/panels.tsx`, `app/globals.css`):
  - Timeline 패널을 없애고 **Timeline / Playback 패널의 첫 탭(기본)** 으로 이동. 탭: Timeline · Playback · Run Setting · Stamp Playback · Global Animation Playback.
  - 패널 배치 7칸 → 6칸(아래 행: Attributes 4칸 / Timeline·Playback 5칸 / Color 3칸), 행 높이 기본 280px(넓은 화면 300, 좁은 화면 260), 경계선 드래그 최소 100px.
  - 탭 줄은 패널 맨 아래에 고정(내용만 스크롤) → Run Setting 등을 선택해도 탭이 올라가지 않음.
  - 저장 키 갱신(`cg-layout-v3`, `cg-rows-v4`)으로 이전 배치/높이 설정은 무시되고 새 기본값 적용.
- 검증: `tsc --noEmit` 통과. Chrome(CDP)로 1920×1080 에서 5개 탭 모두 탭 줄 위치 동일(top 1022) 확인, 화면 캡처 확인.
  캔버스 영역 높이 570px.

### [cg-editor] 미디어/프로젝트 선택 목록 정렬 수정
- 증상: 미디어 선택 목록에서 항목이 세로로 눌려 파일명과 용량(두 줄)이 잘려 보임.
- 원인: `.savedlist`(flex 세로 + max-height 280px) 안의 버튼이 항목이 많으면 flex 로 줄어듦(높이 25px).
- 수정(`app/globals.css`): 버튼 `flex-shrink:0`, 최소 높이 48px, 세로 가운데 정렬, 긴 파일명은 말줄임, 목록 최대 높이 `min(420px,55vh)`. 프로젝트 열기 목록에도 같이 적용됨.
- 확인: Chrome(CDP)으로 긴 파일명·11개 항목 캡처 확인(항목 높이 25px → 약 58px, 이름/용량 모두 표시).

### [cg-editor] 파일 메뉴 정리 + 텍스트 영역 자동 맞춤
- 파일 메뉴: 새 프로젝트 / 프로젝트 열기 / 프로젝트 저장 / PNG 내보내기 (툴바 설명·저장 대화상자 제목도 같은 이름). 동작은 그대로.
- 텍스트 영역 자동 맞춤(`app/text-render.ts` `fitText`, `app/model.ts` `Item.autoSize`, `app/page.tsx`):
  - 증상: 새 텍스트가 1400×180(또는 화면 오른쪽 끝까지 × 160)으로 만들어져 글자보다 영역이 너무 큼.
  - 새로 만드는 텍스트/시계/계수기는 글자 크기에 맞춰 시작(예: 기본 자막 100px 글자 → 772×117). 글자·글꼴·크기·자간·외곽선 등을 바꾸면 영역도 다시 맞춤.
  - W/H 를 직접 바꾸거나 핸들로 크기를 조절하면 자동 맞춤이 해제됨. 편집 메뉴 "텍스트 영역 맞춤"으로 다시 맞춤(선택한 개체).
  - 폭 여백은 8 미만으로 줄이면 안 됨(drawText 가 `줄폭+글자폭 > w-8` 이면 줄바꿈 → 마지막 글자가 사라짐). 그림자 번짐은 영역에 포함하지 않음.
  - 기존 프로젝트의 텍스트는 `autoSize:false` 라 그대로 유지됨.
- 확인: `tsc --noEmit` 통과, Chrome(CDP)로 삽입→문자 결과 캡처 확인.

### [cg-editor] 프로젝트 저장 / 새 이름으로 저장 분리
- 파일 메뉴: 새 프로젝트 / 프로젝트 열기 / **프로젝트 저장** / **프로젝트 새 이름으로 저장** / PNG 내보내기.
- **프로젝트 저장**(메뉴·툴바·Ctrl+S): 이름을 묻지 않고 `bin/project/<프로젝트 이름>.json` 에 바로 저장(이미 저장/열어 온 프로젝트는 덮어씀).
  한 번도 저장하지 않은 새 프로젝트가 같은 이름의 기존 파일과 겹칠 때만 덮어쓰기 확인을 묻는다(이름은 묻지 않음).
- **프로젝트 새 이름으로 저장**(메뉴·Ctrl+Shift+S): 이름 입력 창을 띄우고, 이미 있는 이름이면 덮어쓰기 확인. 창에는 "PC에 JSON 파일로 저장"도 있음.
- 이전의 "날짜를 붙여 새 파일로 저장" 버튼과 3개 저장 버튼은 제거.
- 확인(Chrome CDP): 새 이름으로 저장 → 이름 창 표시 → 임시 이름 저장 성공, 이어서 프로젝트 저장 → 창/확인창 없이 파일 갱신.
  테스트 임시 파일은 삭제(기존 `test.json`, `test1.json`, `자막프로젝트.json` 은 그대로).

### 커밋/푸시 (2026-10-01)
- `c1693c7` 실행엔진(cg-streamer) 구현, 에디터 화면/저장 구조 개편 → `origin/main` 푸시 (`040d475..c1693c7`, 31개 파일).
  - 앞서 100MB 초과 파일(`doc/cef/cef_custom_130_arm64.tar.gz`) 때문에 거부된 푸시는 커밋을 합치고 해당 파일을 제외해 `eff7b0c` 로 해결.
- 커밋하지 않은 것: `src/cg-editor/tsconfig.tsbuildinfo`(빌드 산출물), `bin/project/` 의 사용자 테스트 파일(`test.json`, `test1.json`, `자막프로젝트.json`),
  `bin/media/` 의 샘플 미디어(`.gitignore` 로 제외, `.gitkeep` 만 추적).
- 푸시 전에 발견/조치:
  - 사용자 커밋 `040d475 delete bin/web` 이 같은 경로를 지우며 `bin/web/player.html` 이 디스크에서 사라짐 → 같은 내용으로 재생성해 커밋.
  - 줄바꿈 자동 변환(`core.autocrlf=true`) 때문에 `.sh` 가 보드에서 CRLF 로 깨질 수 있어 `.gitattributes` 추가(`*.sh eol=lf`, `*.bat eol=crlf`).
- 남은 일: **C++ 변경(`main.cpp`, CMake)은 Windows 에서 빌드하지 못해 컴파일 미검증** → 보드에서
  `cd src/cg-streamer && cmake --preset rk3588 && cmake --build --preset rk3588` 후 `bin/cg-streamer --run --project=project/<이름>.json` 확인 필요.
  브라우저에서의 미디어 선택·저장/열기 실제 조작은 CDP 스크립트 수준으로만 확인.

## 2026-10-01 (이어서)

### [cg-editor] 다크 테마 적용 (대시보드/팝업 참고 화면 계열)
- 참고: 블랙/네이비 바탕 + 카드형 패널 + 남보라 헤더 + 파란 주 버튼 + 초록 강조, 팝업은 둥근 모서리(16px)·"✕ 닫기"·하단 작은 고정폭 안내줄.
- `app/globals.css`:
  - 기존 회색 계열 색(배경/글자/테두리 90곳)을 네이비 톤으로 일괄 변환(채도 있는 색과 이미 어두운 색은 유지).
  - 파일 끝에 "Dark theme polish" 층 추가: 색 변수(`--bg/--card/--head/--accent` 등), 패널(둥근 카드, 남보라 제목줄), 버튼/입력/탭(활성=파란색), 메뉴·드롭다운, 스크롤바, 상태줄.
  - 팝업 다이얼로그: 배경 흐림 + 어둡게, 둥근 모서리, 제목줄 "✕ 닫기", 목록 항목 카드, 하단 안내줄 고정폭 글꼴.
- 원래 색으로 되돌리려면 `git checkout -- src/cg-editor/app/globals.css` (커밋 전 기준).
- 확인: Chrome(CDP) 1920×1080 전체 화면, 새 이름으로 저장·미디어 선택 다이얼로그 캡처 확인. 캔버스에 그려지는 자막 색과 cg-streamer 출력에는 영향 없음(에디터 UI 색만 변경).

### [cg-editor] 화면/저장 UI 정리 (이어서)
- 제목: "Huton CG Editor"(화면 제목줄·브라우저 탭). 문구에서 "JSON" 제거(화면에 보이는 글자만, `.json` 확장자·코드는 그대로).
- 파일 메뉴 "프로젝트 다른 이름으로 저장"(이름 창), "프로젝트 저장"은 이름 없이 바로 저장. 프로젝트 열기에서 PC 파일 선택/새로고침/붙여넣기 제거, **검색 칸** 추가(이름 필터, Enter 로 첫 결과 열기, 전체 개수 표시).
- 패널: Attributes / Effects 폭 조정(24칸 그리드, 가운데 블록 고정 폭), 영상·음성 "시작 오프셋" 줄바꿈 방지(입력 칸이 남는 폭에 맞춰 축소), 텍스트 영역 자동 맞춤.
- 타임라인: 현재 위치(playhead) 청록색, 선택된 줄/클립 글자 대비 개선(파란 선택색, 종류별 클립 색).
- Effects 탭 오른쪽 미리보기 칸: 안쪽 색을 네이비 톤으로, 높이를 줄임(칸 41→35px). 이 탭의 다른 요소(목록·입력 칸 정렬)를 함께 바꾼 시도는 "이상해졌다"는 피드백으로 원복.
- 예제: `bin/project/가로스크롤-예제.json` — 뉴스 자막 형태의 가로 스크롤(Crawl) 텍스트(오른쪽 끝에서 출발, 글 폭 6228px / 26초). 실행엔진(cg-runtime)에서 3·12·22초 캡처로 동작 확인.
- 남은 제안: 열 때 미디어 파일 존재 확인 경고, 도움말 대화상자의 하단 상태 문구 정리, 끊김 없는 무한 가로 스크롤(새 효과) — 사용자 답변 대기.

### [cg-editor] In/Out 선택 표시, Run Setting 안내 문장 간격
- Effects 탭 In/Out 버튼: 공통 버튼 색 규칙이 `.active` 색을 덮어써서 선택 구분이 안 되던 문제 수정(선택=파란색, 미선택=어두운 회색). 같은 종류의 `.checks`/`.facebuttons` 토글에도 적용.
- Run Setting 탭: "Manual은 페이지 끝에서 멈춥니다…" 안내 문장을 버튼 줄과 분리해 한 줄 전체를 쓰고 두 줄(30px) 간격을 둠.

### [cg-editor] 프로젝트 삭제, 팝업/툴바/미리보기 수정
- 프로젝트 열기 목록에 **삭제 버튼**(휴지통) 추가: 확인창 후 `bin/project/<이름>.json` 삭제, 현재 열려 있는 프로젝트는 삭제 불가, `bin/media` 는 지우지 않음.
  파일 서버에 `DELETE /projects/<이름>` 추가(이름 정리·없는 파일 404). 기존 서버 프로세스는 재시작해야 적용됨(`stop`/`start`).
- 프로젝트 열기 팝업에서 패널 경계선(`.rowsplit`, z-index 20)이 선택되던 문제: 팝업 배경 z-index 100, 우클릭 메뉴 101 로 상향.
- 툴바 첫 번째 버튼을 "새 페이지" → **"새 프로젝트"** 로 변경(파일 메뉴와 동일 함수 `newProject`, 수정 사항이 있으면 확인창). 새 페이지는 Page List "+ 추가"와 삽입 메뉴에 있음.
- Timeline Preview 가 2페이지로 넘어가던 문제: Run Setting 의 Apply to Playback / All File Playback 으로 켜진 "이어서 재생(sequence)" 상태를 단일 페이지 미리보기(Timeline Preview, Effects Preview, 보기>미리보기, F5)가 끄지 않던 것이 원인 → 미리보기 시작 시 `sequence.current=false`.
  재현(Apply 후 Preview → 0.7초 만에 Page 2)과 수정 후(Page 1 유지) 확인.

### 순위/기록 예제 프로젝트, 이동(moves) 기능, 메뉴 아이콘
- **개체 이동(`moves`) 기능** (에디터 `app/model.ts`·`editor-canvas.tsx`, 실행엔진 `bin/web/cg-runtime.js` 공용):
  `Item.moves=[{t:시작, dur:걸리는 시간, x?, y?}]` — 지정한 시각에 새 위치로 부드럽게(easeInOutCubic) 이동. 순위 변동 등에 사용.
  normalizeProject 가 moves 를 검증해 보존(최대 500개, 값 범위 제한). 편집 UI 는 없음(JSON 작성, 재생/저장은 지원).
- 예제 프로젝트(`bin/project/`): `수영-기록.json`(남자 100m 자유형 1~10등 기록, 청록), `육상-기록.json`(남자 100m 1~10등, 주황),
  `종합순위-변동.json`(5라운드에 걸쳐 점수가 바뀌고 순위가 변하면 줄이 새 자리로 이동, ▲/▼ 변동 표시, 30초 반복).
  기록판은 `linkName`(r1_name, r1_time …)으로 HTTP `PUT /text/<이름>` 갱신 가능(순위표에서 갱신 동작 확인). 샘플 데이터는 가상.
  경기 스코어보드/리그 순위표/이름·점수 순위표는 방향이 바뀌어 삭제.
- 메인 메뉴 항목별 **아이콘**(`app/menu-icons.tsx`), 메뉴 글자 왼쪽 정렬·폭 자동(긴 항목 줄바꿈 방지).
- 확인: `tsc --noEmit` 통과, 실행엔진/에디터 캡처(이동 전·중·후), moves 보존(130개 개체 일치), 메뉴 4종 캡처.
- 남은 제안(답변 대기): 실시간 순위 변경 HTTP 명령, moves 편집 UI, 기록이 흐르며 순위가 바뀌는 레이스형, 열 때 미디어 확인 경고, 도움말 하단 상태 문구 정리, 창 메뉴 패널 이름 한글화.

### 생활정보 문자방송 예제 (2페이지)
- `bin/project/생활정보-문자방송.json`: 글자 중심의 문자방송. 머리띠(제목·현재 시각 시계), 페이지 제목/쪽수, 2×2 글자 구역(구역 제목 색 + 줄글), 아래 띠(노란 "생활 알림" 라벨 + 흐르는 안내 자막).
  1페이지 날씨·대기질·생활지수·교통, 2페이지 긴급 연락처·건강 수칙·분리배출·공공시설. 수치/내용은 예시(자막에도 표기).
- 재생: 페이지당 18초, 자동 반복(loops 100). 전환은 다음 페이지가 오른쪽에서 밀려 들어오는 `move`(0.7초), 이전 페이지는 `fade` 로 퇴장(0.5초).
  수동으로 넘기려면 Run Setting Mode=Manual. 에디터의 Timeline Preview 는 단일 페이지라서 여러 페이지 확인은 Run Setting 의 All File Playback.
- 확인: 실행엔진 캡처(1페이지, 2페이지, 전환 중), 글자 폭 측정으로 자막 길이 설정(3102/3159px).

### CEF 130 rkmpp 패치 빌드 적용, cg-streamer 재빌드 (보드에서)
- `/root/work/cef_custom_130_arm64.tar.gz`(서버에서 빌드한 V4L2/rkmpp 패치 CEF)를 `/opt/cef/cef_custom_130_arm64`에 설치.
  `make_distrib.py` minimal 패키징을 거치지 않은 원시 빌드 산출물이라 `cmake/`, `CMakeLists.txt`, `include/cef_version.h`,
  `include/cef_config.h`, `include/base/internal/cef_net_error_list.h`, `Release/chrome-sandbox`, `Release/libvulkan.so.1` 이 빠져 있었음 →
  기존 공식 130.1.16 배포본(동일 커밋 해시 `5a7e5ed`, 버전 문자열 일치 확인)에서 보강.
- `~/.bashrc`의 `CEF_ROOT`를 새 경로로 변경, `cg-streamer` 재빌드.
- 확인: `libcef.so`에 `/dev/video-dec` 문자열 5개(기존 공식본은 0개) → V4L2 패치 반영 확인. `cg-streamer` 실행 시 zygote/gpu-process/renderer 등 CEF 하위 프로세스 정상 기동,
  `--run --out=test.ts --seconds=10`로 1920×1080/60fps/드롭 없음 H.264 출력 확인(ffprobe).
  실제 RK3588 GPU로 mp4 `<video>` 하드웨어 디코딩이 되는지(`mpp_service` fd)는 미검증 — `cef_server_build.md` 5단계 참고.

### [cg-editor] Cloudflare/vinext 제거 → 순수 Next.js 전환
- 배경: `npm run dev`(vinext + `@cloudflare/vite-plugin`, 로컬 workerd 에뮬레이션)가 보드(Debian 11, glibc 2.31)에서 `workerd`
  요구 glibc(≥2.35)를 못 맞춰 기동 즉시 죽고, miniflare가 죽은 프로세스에 쓰다 `EPIPE`. 이 편집기의 로컬(보드) 사용에는
  ChatGPT Sites 호스팅 전용 기능(D1/R2 클라우드 저장, Workers 바인딩)이 불필요해 Cloudflare 레이어를 통째로 제거.
- 삭제: `vite.config.ts`, `build/`(sites-worker, connector-preview 등 Sites 플러그인 전용), `lib/storage.ts`, `db/`, `drizzle.config.ts`, `drizzle/`,
  `cloudflare-env.d.ts`, D1/R2 의존 `app/api/projects`·`app/api/media` 라우트(로컬 저장은 `files-server.mjs`가 이미 처리, 안 쓰이던 코드였음).
- `package.json`: `dev`/`build`/`start` → `next dev`/`next build`/`next start`, `@cloudflare/*`·`vinext`·`vite`·`wrangler`·`drizzle-*` 등 devDependencies 제거.
  `tsconfig.json`에서 `@cloudflare/workers-types` 제거.
- `start.sh`/`build.sh`의 옛 wrangler 흔적도 수정: Next.js 16 CLI는 `--host`가 아니라 `--hostname`(dev/start 둘 다), 빌드 산출물은 `dist/`가 아니라 `.next/`.
- 부수적으로 발견: `.next`/`.vinext`/`.wrangler`/`next-env.d.ts`가 이전에 root 권한으로 생성돼 있어 pi 권한 실행 시 권한 오류 → 정리.
  (교훈: 이 보드에서 `npm run dev`를 터미널에서 직접 실행하면 root로 뜨기 쉬우므로 꼭 `./start.sh` 사용할 것, 그래야 파일 서버도 같이 뜸.)
- 확인: `./start.sh`로 에디터(`:5173`)·파일 서버(`:8081`) 정상 기동, `tsc --noEmit` 통과. `app/` 쪽 에디터 코드는 처음부터 순수 Next.js App Router
  코드라 수정 불필요했음.

### [cg-editor] LAN 접속, 보안 컨텍스트 문제 수정
- `crypto.randomUUID()`는 HTTPS/localhost 같은 "보안 컨텍스트"에서만 동작 → 보드를 LAN IP(`http://10.10.10.56:5173`)로 열면
  `crypto.randomUUID is not a function`으로 새 프로젝트 생성이 깨짐. `app/model.ts`의 `uid()`를 `crypto.getRandomValues`(보안 컨텍스트 제한 없음)
  기반으로 교체.
- `.env`의 `CG_EDITOR_HOST=0.0.0.0` 활성화(파일 서버도 같은 주소로 열리게). LAN IP로 열면 Next.js 16이 기본으로 HMR 리소스를 cross-origin
  차단 → `next.config.ts`에 `allowedDevOrigins: ['10.10.10.56']` 추가.
- 확인: 보드 LAN IP로 에디터·파일 서버 접속, 프로젝트 열기(`bin/project` 목록 정상 응답) 확인.

### cg-streamer HTTP 컨트롤 중계("송출 제어" 탭) 신설
- 배경: `cg-streamer`는 `--run` 모드에서 HTTP 컨트롤 서버(기본 `127.0.0.1:5555`, play/next/prev/goto/stamp/global/text/quit/status)가
  항상 켜져 있지만, 브라우저가 CORS 때문에 직접 호출할 수 없고 에디터 쪽에 이를 호출하는 코드도 없었음.
- `scripts/files-server.mjs`에 `/ctl/*` 프록시 추가(`CG_CTL_PORT`/`CG_CTL_HOST`/`CG_CTL_TOKEN` 환경변수, 본문·메서드 보존,
  토큰은 프록시가 붙임) → 같은 오리진(파일 서버)을 거쳐 CORS 회피. `app/files.ts`에 `ctlStatus()`/`ctlCommand()` 클라이언트 함수.
- `app/stream-control.tsx`(신규, `useChannels`와 같은 훅 패턴) + `app/page.tsx`에 **"송출 제어"** 탭: 상태 표시(1.5초 폴링: 프로젝트명·페이지·재생여부·시간),
  Play/Pause/Stop/Cut/Prev/Next/Skip/Clear, 페이지 이동, 엔진 종료(확인창).
- 버그 하나 발견·수정: 프록시가 `req.pipe(preq)`로 본문을 스트림 전달하면서 `Content-Length`를 안 넘겨 Node가 chunked 로 보냈는데,
  `cg-streamer`의 단순 HTTP 서버는 chunked 를 모르고 Content-Length 만 봐서 본문이 빈 것으로 읽힘 → `Content-Length` 헤더를 그대로 중계하도록 수정.
- 확인: curl로 next/goto/pause 등을 프록시 경유로 보내 cg-streamer 상태가 실제로 바뀌는 것 확인(직접 호출과 응답 동일).

### cg-streamer: reload / switch(다른 프로젝트로 전환) 명령, --run 중복 실행 방지
- **reload**(`POST /reload`): project 파일을 디스크에서 다시 읽음. `cg-streamer`는 시작할 때 프로젝트를 한 번만 읽고 다시 읽는 수단이
  없었음(에디터에서 저장해도 떠 있는 송출에 반영 안 됨) → `bin/web/cg-runtime.js`에 `reloadProject()` 추가, 네이티브 `load` cefQuery(기존에 이미
  디스크에서 다시 읽게 구현돼 있던 것)를 재사용.
- **switch**(`POST /switch`, 본문=UTF-8 평문 프로젝트 이름): `bin/project`의 다른 프로젝트로 통째로 전환. `g_project` 경로를 바꾼 뒤 reload.
  경로 구분자/상위 경로/제어문자 검증(`SafeProjectName`, 한글 프로젝트명은 허용), 없는 프로젝트는 404.
- 두 명령 모두 처음엔 "1페이지·정지 상태"로 리셋했는데, 사용자가 반영이 "안 되는 것처럼" 두 번 헷갈려함(실제론 반영됐지만 재생 중이 아니라
  화면이 그대로/검은 화면) → `--autoplay`로 띄운 엔진이면 reload/switch 후 바로 재생되게 수정(URL의 `autoplay=1` 체크, `rangeStart()`로 시작).
  수동 제어(비 `--autoplay`)는 기존대로 정지 상태 유지.
- **`--run` 중복 실행 방지**: 같은 UDP 목적지/컨트롤 포트로 여러 인스턴스가 뜨면 출력이 섞여 비트레이트가 이상해짐 → `/tmp/cg-streamer.run.lock`
  파일락(`flock`, 비정상 종료해도 커널이 자동 해제). 두 번째 `--run` 은 즉시 "이미 실행 중" 메시지와 함께 종료(exit 1).
- cg-editor "송출 제어" 탭에 전환용 드롭다운(`bin/project` 목록) + "다른 프로젝트로 전환", "저장한 내용 다시 불러오기(Reload)" 버튼 추가(`app/files.ts`
  `ctlSwitch()`).
- 버그 발견·수정: `useStreamControl`의 "언마운트 후 setState 방지" `mounted` ref 가드가 Fast Refresh(핫리로드)로 `false` 에 고정돼버리면
  그 뒤 모든 액션의 `finally{setBusy(false)}`가 스킵되어 버튼 전체가 영구 disable 되는 버그 → React 18+ 는 언마운트 후 setState가 안전(조용히 무시)하므로
  가드 자체를 제거.
- disable/활성 버튼 시각 구분 개선(`app/globals.css` `.streamcontrol`): disable 은 회색조+점선 테두리+`cursor:not-allowed`, 활성은 테두리 강조.
- 확인: `--run` 중복 실행 시도 → 즉시 거부(exit 1) 및 정상 종료 후 재시작/강제종료(`kill -9`) 후 재시작 모두 정상 확인. reload/switch 를 경로 탈출·존재하지
  않는 프로젝트명으로 시도 시 올바르게 거부. 8081 프록시 경유로 20회 연속 랜덤 프로젝트 전환(0.3초 간격) 전부 성공, 프로세스 메모리/개수 이상 없음.
  `tsc --noEmit` 통과.
- 남은 일: `main.cpp`/`cg-runtime.js` C++·JS 변경은 재빌드·재시작해야 반영됨(핫리로드 대상 아님) — 수정할 때마다 `cg-streamer` 재시작 필요한 걸 계속 깜빡해서
  디버깅이 늘어졌음, 다음엔 변경 직후 바로 재시작·재검증 습관화.

### cg-streamer: reload 후 네이티브 영상이 눌러붙는 버그 수정
- 증상: 영상이 있는 페이지(예: `자막프로젝트.json` 2페이지의 `girsday.mp4`)를 거쳐 reload/switch 한 뒤에는, 영상이 없는 페이지/프로젝트로 돌아가도
  `[stat]` 로그의 `video=on`이 계속 남고 `under`(프레임 반복 카운터)가 멈춘 채 고정됨 → 비트레이트·CPU 이상, "송출이 안 되는 것 같다"로 체감.
- 원인: `reloadProject()`(`bin/web/cg-runtime.js`)가 `nativeVideo.key`를 미리 `''`로 리셋해뒀음. 매 프레임 도는 `syncVideo()`는 "이전 키와 다르면
  video:stop/play 를 보낸다" 는 비교로 동작하는데, 새 프로젝트도 영상이 없으면 desired 영상 키가 똑같이 `''`가 되어 "변화 없음"으로 오판 → 이전 프로젝트의
  네이티브 MPP 비디오 디코더를 끄는 `video:stop` 이 안 나가고 그대로 눌러붙음.
- 수정: `reloadProject()`에서 `nativeVideo.key` 리셋 줄 제거 — `syncVideo()`의 자연스러운 비교에 맡김.
- 확인: cg-streamer 재시작 후 영상 있는 페이지(2) → 영상 없는 프로젝트("가로스크롤-예제")로 전환 → `[stat]` 로그가 `video=on`에서 `video=off`로 정상
  전환되고 `under` 카운터도 다시 정상적으로 증가하는 것 확인.
- 참고(답변): 프로젝트의 `"video"` 아이템은 CEF `<video>` 태그가 아니라 네이티브 MPP 디코딩(`video_source.cpp`) + RGA 합성 경로를 씀
  (`cg-runtime.js` 는 video 타입을 캔버스에 그리지 않고 건너뜀). 이번에 적용한 CEF rkmpp(`<video>` 태그용 V4L2 패치)는 이 경로와 무관 — 이 기능은 패치 없이도 그대로 동작.

### --accel(OnAcceleratedPaint dmabuf) 실기 테스트 — 동작 안 함, 원복
- `rk3588-accel` 프리셋(`ENABLE_ACCEL_PAINT=ON`)으로 재빌드 후 `--accel --out=test.ts --seconds=10`로 테스트.
- 결과: GPU 프로세스가 초기화 단계에서 죽음(`libGL error: failed to load driver: rockchip` → `Exiting GPU process due to errors during initialization`).
  accel 경로는 GPU 텍스처 공유가 필수라 GPU 프로세스가 없으면 아예 동작 불가 — `test.ts` 0바이트(프레임 한 개도 안 나옴).
  (OnPaint 소프트웨어 경로는 이 GPU 실패와 무관하게 정상 동작 — 지금까지 써온 기본 경로는 영향 없음.)
- 원복: `bin/cg-streamer`가 다른 빌드 디렉터리(`build/rk3588-accel`)에 의해 덮어써진 상태라 `build/rk3588`가 "no work to do"로 재빌드를 건너뜀 →
  `bin/cg-streamer` 를 직접 지우고 `rk3588`(비-accel) 프리셋으로 강제 재링크, `--accel` 플래그를 줘도 OnPaint 로 폴백하는 것까지 확인.
- 결론: 이 보드/환경에서는 Mesa rockchip DRI 드라이버가 없어서 `--accel`을 못 씀. 정식 지원하려면 별도 드라이버 설치/설정 필요(미조사).

### [cg-editor] HDMI 입력(라이브) 오브젝트, 화면 전체 크기 맞춤
- HDMI RX 라이브 테스트용으로 프로젝트에 `src:"hdmirx"`인 video 아이템을 넣는 UI가 없었음(기존 "동영상"은 `bin/media` 파일만 선택 가능) → 추가:
  - `app/page.tsx` `addHdmiVideo()`: 파일 선택 없이 바로 전체화면(1920×1080) video 아이템 생성. 삽입 메뉴·툴바(Cast 아이콘)에 배치.
  - `app/model.ts` `normalizeProject`: src 검증 정규식이 `hdmirx`/`/dev/videoN`을 "unsupported media"로 거부하던 것 수정(저장 후 재오픈 시 사라지는 것 방지).
  - `app/editor-canvas.tsx`: 디자인 캔버스에서 hdmirx 아이템은 실제 로드를 시도하지 않고 "HDMI 입력 (라이브)" 플레이스홀더로 표시(실제 재생은 cg-streamer 에서만 확인 가능).
- 테스트 중 위치가 살짝 밀려(x=-5,y=-9) RGA가 매 프레임 "Illegal dst rect"(좌표 음수) 에러를 내며 합성 실패 → 화면엔 "처음엔 나오고 그 다음부터 안 보임"으로 체감.
  위치를 0,0,1920×1080으로 되돌리고 `reload`로 확인(에러 사라짐).
- 재발 방지로 **"화면 전체 크기로"** 기능 추가(`fitFullscreen`, 툴바 Maximize2 아이콘 + 우클릭 메뉴): 선택한 오브젝트를 정확히 x:0,y:0,w:1920,h:1080 으로 맞춤.
- HDMI RX 자체(`hdmirx_source.cpp`)는 로그상 재연결이 여러 번 정상적으로 성공(`stop`→`start`→`signal 3840x2160 59.94fps`)했지만, 한 번은 신호를 잡자마자 바로
  꺼지고 이후 페이지에 계속 있었는데도 재연결이 안 된 경우를 로그에서 발견 — 원인 미해결, 재현 시 추가 조사 필요(네이티브→JS로 연결 끊김이 전달 안 돼서
  `syncVideo()` 의 키 비교가 재연결을 트리거 못 하는 경로가 있을 가능성).

### 로그 정리, 보드 디스플레이(HDMI 출력) 확인, 키오스크 스크립트
- `bin/log/` 폴더 신설, 테스트하며 쌓인 `.stream*.log` 찌꺼기 정리. `bin/*`가 이미 `.gitignore`라 `bin/log/`는 별도 설정 없이 자동 제외됨.
- 보드에 HDMI 출력 포트가 2개(물리 라벨 "1"/"2") 있는데, DRM 커넥터명(`card0-HDMI-A-1`/`-2`)과 실제 매핑이 케이블 연결마다 바뀌는 것처럼 보임(원인 미확인 —
  재현 조건 불명확). 모니터가 1대뿐이라 "editor/송출 분리 출력"은 지금은 확인 불가, 모니터 2대 연결 시 재확인 필요.
  해상도가 800x600으로 낮게 고정돼 있던 것은 `xrandr --output <출력> --mode <최대모드>`로 수정(이 모니터는 1920×1080 지원 안 함, 최대 1368×768).
- `cg-streamer` 자체는 런타임에 `/opt` 가 필요 없음(빌드 시 `bin/`에 CEF 런타임 파일을 전부 복사해 두고, RPATH도 `.` 를 `/opt/...` 보다 먼저 찾음 —
  `/opt/cef/...`는 재빌드할 때(`CEF_ROOT`)와 `bin/libcef.so`가 없어졌을 때의 폴백 용도). `/opt/chromium.org/...`는 별개로 키오스크용 보드 시스템 브라우저.
- `bin/start.sh`/`bin/stop.sh` 신설: cg-editor + cg-streamer(UDP 송출, 로컬 미리보기 없음) + Chromium 키오스크(cg-editor 화면)를 한 번에 올리고 내림.
  `CG_PROJECT`/`CG_UDP`/`CG_EDITOR_PORT` 환경변수로 기본값(자막프로젝트, 10.10.10.18:1234, 5173) 변경 가능. `.gitignore`에 `!bin/start.sh`/`!bin/stop.sh` 추가.

## 2026-10-01 (이어서 2) — cgsetup.cfg 신설, 가로스크롤 끊김 원인 추적 → 진짜 Mali GPU 가속 활성화

### cg-streamer: cgsetup.cfg 로 송출 설정 분리
- `bin/cgsetup.cfg` 신규(기본값 없으면 조용히 스킵): `output`(1=HDMI만/로컬 전체화면·인코딩 없음, 2=UDP만·기본, 3=HDMI+UDP 동시),
  `udp_ip`/`udp_port`, `fps`(인코더 rc·EncodeLoop 틱·CEF 페인트 fps 공통 기본값, 기본 60). `--setup=경로`로 다른 파일 지정 가능,
  커맨드라인 인자가 항상 cfg보다 우선.
- `kFps` 상수를 런타임 변수 `g_fps`로 교체(여러 곳에서 치환), `g_paint_fps` 기본값도 `g_fps`에서 파생.
- `.gitignore`에 `!bin/cgsetup.cfg` 추가(`bin/*` 예외).

### CEF 자체 그리기 속도(SendExternalBeginFrame/EBF) 실험 — 효과 없음, 원인 아니었음
- 가설: CEF 내부 60Hz 타이머와 EncodeLoop 60Hz 타이머가 서로 어긋나 프레임을 건너뛰어 가로스크롤이 끊겨 보이는 것 아닐까.
- `CG_EBF`(`-DENABLE_EXTERNAL_BEGIN_FRAME=ON`) 빌드 옵션 추가: `windowless_frame_rate` 대신 `EncodeLoop` 틱에서 `SendExternalBeginFrame()`을
  직접 호출. 구현 중 두 가지 실버그 발견·수정: (1) `!g_ready` 로딩 대기 중에도 BeginFrame을 계속 보내야 로딩이 끝남(안 그러면 영원히 교착),
  (2) `SendExternalBeginFrame()`은 UI 스레드에서만 호출 가능 — `CefPostTask(TID_UI, ...)` + 중복 요청 방지 플래그로 수정.
- 실측 결과: 지터버퍼 효과(`drop=0 under=0`)는 깨끗하게 나왔지만, **체감 끊김은 이전과 동일**("똑같다"/"별 차이 없다" 반복 확인).
- 결론: EBF는 기능상 정상 동작하지만 가로스크롤 끊김의 원인이 아니었음. 실행 파일(`cg-streamer-ebf`)은 삭제, 소스의 `CG_EBF` 빌드
  옵션(기본 OFF)만 참고용으로 남김.

### OnPaint 원본 직접 캡처 진단(cef_dumper) — 두 번 수정이 필요했던 측정 함정
- 목적: RGA/MPP/UDP를 전혀 안 거치고 CEF가 OnPaint로 내놓는 원본 BGRA를 그대로 mp4(VFR, 도착 시각 그대로)로 떠서 끊김이 CEF 자체
  문제인지 이후 단계 문제인지 구분. `cef_dumper.{h,cpp}` 신규, `main.cpp`에 `--dump-onpaint=`/`--dump-seconds=`/`--page=`(테스트용 다른
  html 로드) 플래그 추가.
- 1차 시도(동기 인코딩): OnPaint 콜백 안에서 곧바로 `sws_scale`+`avcodec_send_frame`(libx264)까지 동기 처리 → 8초 캡처에 88장(~11fps)
  밖에 안 나옴. 사용자가 실제로 영상을 보고 "그렇다(끊겨 보인다)"고 확인.
- 2차 시도(비동기로 수정 후 재측정): 느린 인코딩이 OnPaint 자체를 블로킹하는 게 측정을 왜곡하는 것 아닌가 싶어, PushFrame은 가벼운
  메모리 복사만 하고 실제 sws_scale/libx264 인코딩은 별도 워커 스레드(큐+조건변수)로 분리하도록 재작성. 재측정 결과도 ~14~16fps로
  거의 동일 — `--gpu`/cfg 로드 유무를 바꿔도 변화 없음.
- **측정 함정 결론**: 이 덤프 테스트는 `--no-encode`(유휴) 모드에서만 돌렸는데, 이 보드(ARM, CPU 주파수 스케일링)는 시스템이 거의 일을
  안 하면 클럭을 낮춰서 CEF 자체도 덩달아 느려짐 — 즉 "CEF가 느리다"가 아니라 "유휴 상태의 보드가 느리다"를 측정하고 있었음. 실제
  운영 조건(인코딩 켜짐, MPP/RGA가 CPU/GPU를 바쁘게 돌림)에서 재는 `[stat] paint=.../s` 가 진짜 신뢰할 수 있는 숫자. 진단 코드는
  `main.cpp`에서 `#if 0`로 비활성화, 코드만 보존(`cef_dumper.*`는 비동기 버전으로 남겨둠).

### 가로스크롤 효과를 캔버스 대신 CSS transform/animation으로 — 시도했으나 퇴행 있어 원복
- 실측(`bin/web/test-css-scroll.html`로 A/B): 캔버스 매프레임 재그리기 paint≈74~86/s, CSS 버전 paint≈105~120/s(최대 1.6배). 메인
  스레드 지연에 덜 민감할 거라 기대.
- `cg-runtime.js`에 `isCssCrawl()`/`syncCrawlOverlays()` 적용(type=text, effect가 직접 crawl/roll, outEffect 없음, runs 없음인 경우만
  DOM 오버레이로 전환) → 실측 성능은 확보했지만 사용자가 실제로 보고 **글씨체가 달라짐**(캔버스의 kerning/space/textWidth 커스텀
  로직과 레이어별 외곽선을 `-webkit-text-stroke`로 재현 못함) **레이어 순서 깨짐**(DOM을 body 맨 뒤에 붙여서 항상 캔버스 전체보다
  위에 뜸 — "속보" 라벨이 가려짐)을 확인 → 전체 되돌림(커밋 `b6d4d2c`).
- 버그 하나 더 발견(되돌리기 전에 수정까지 했었음, 참고용): `animation-iteration-count:1`+`fill-mode:forwards` 조합은 음수
  `animation-delay`만 바꿔선 재시작이 안 됨(한 번 끝나면 그대로 멈춤) — 페이지가 루프를 돌면 자막이 한 바퀴만 돌고 멈춰버림.
  `animationName`을 `none`으로 바꾸고 강제 reflow(`el.offsetWidth`) 후 원래 이름으로 되돌리는 방식으로 고쳐야 재시작됨.

### 가로스크롤 텍스트를 오프스크린 캔버스에 캐싱 — 유지함
- CSS 전환의 성능 이득(캔버스 재그리기 비용 절감)만 부작용 없이 가져오는 방법: `cg-runtime.js`에 `cachedTextCanvas()` 추가 —
  텍스트/스타일 시그니처가 안 바뀌면 오프스크린 캔버스에 캐싱된 비트맵을 `drawImage()`로 위치만 옮겨 찍음(기존 `drawText()` 그대로
  재사용하므로 글씨체/레이어 문제 없음). 위치 계산(`v.x`)은 기존처럼 매 프레임 그대로 해서 "멈추는" 버그도 구조적으로 발생 안 함.
- 실측: paint 74~86/s → 108~122/s. 커밋 `5b06d94`.

### 근본 원인 발견: `--gpu`로도 사실은 SwiftShader(소프트웨어) 폴백 상태였음
- 위 방법들을 다 적용해도 "체감은 똑같다"는 피드백이 반복 → "이미 공급 과잉(`under=0`)인 상태에서 공급을 더 늘리는" 시도였을 뿐
  근본 원인이 아니었다고 판단, 접근 전환.
- 기본 실행(`--gpu` 없이)은 `main.cpp`에서 `disable-gpu`/`disable-gpu-compositing`이 항상 붙어 CEF가 완전 소프트웨어 렌더링으로
  떨어짐(paint≈11fps) → `--gpu`로 이를 끄면 paint 75~120fps로 개선되는 걸 먼저 확인했었음. 그런데 실제 GPU 프로세스 커맨드라인을
  보니 `--use-gl=angle --use-angle=swiftshader-webgl`로 떠 있었음 — **`--gpu`를 줘도 ANGLE이 SwiftShader로 폴백한 상태**였을 뿐,
  진짜 GPU 가속이 아니었음.
- `cef_server_build.md`(이 보드의 커스텀 CEF 130이 Mali DDK 패치 포함해서 빌드된 문서) 재확인 — 5단계에 정확한 사용법이 이미
  적혀 있었음: `--cef:use-angle=gles-egl`. `--use-gl=egl`(ANGLE 안 거치는 순정 EGL)은 이 CEF 빌드에 아예 컴파일 안 되어 있어
  `gl_factory.cc` 에러로 거부됨, Vulkan 백엔드도 이 보드의 Mali 드라이버(`libmali-valhall-g610-g13p0-x11-gbm`, Vulkan ICD 없음)로는
  불가 — `gles-egl`이 이 커스텀 빌드가 제공하는 유일한 진짜 하드웨어 경로.
- `--cef:use-angle=gles-egl` 적용 결과: GPU 프로세스가 크래시/재시작 없이 한 번에 뜨고, **`paint`가 설정 fps(60)에 정확히 맞춰지며
  `drop=0`**(SwiftShader는 75~120fps 과공급 + 지속적 drop). 사용자 확인: 가로스크롤 끊김 해소, **CPU 사용량 체감 절반으로 감소**.
- `bin/start.sh`에 `--gpu --cef:use-angle=gles-egl` 기본 적용. 커밋 `e9ea8fe`.
- 교훈: 증상(끊김)을 고치려고 여러 렌더링 기법(EBF/CSS/캐싱)을 바꿔치기하기 전에, 먼저 "진짜 GPU를 쓰고 있는지"부터 확인했어야
  시간을 아꼈을 것. `ps aux`로 gpu-process 커맨드라인의 `--use-gl`/`--use-angle` 값을 확인하는 게 가장 빠른 1차 점검 포인트.

## 2026-10-01 (이어서 3) — 업스트림 병합, start.sh 기본 production, 패널 배치, stop.sh 보강

### git 업스트림 받기 (충돌 해결)
- 원격의 cg-editor 성능 개선 커밋(`a7a77b6`)을 `git pull --rebase --autostash`로 받음. 로컬 수정과 `page.tsx`/`stream-control.tsx`가 충돌.
  업스트림(attrPatch 등 성능 개선, `document.hidden` 폴링 중단)을 기준으로 두고 로컬 변경분(`char-stylegrid`, 'Stream Control' 탭 이름·맨 앞 순서,
  "Refresh list" 주석)만 다시 얹어서 해결. 충돌 마커 없음·`tsc --noEmit` 통과 확인.

### cg-editor/start.sh 기본 모드를 production 으로
- `./start.sh` → `npm start`(빌드 결과 `.next/` 사용, `./build.sh` 선행 필요). 개발 서버는 `./start.sh --dev`. `--prod`는 호환용으로 유지.
- 도움말의 옛 `dist/` 표기를 `.next/`로 정정. `bin/start.sh`는 옵션 없이 호출하므로 자동으로 production 으로 뜸.
- 포트는 `.env.production`의 `CG_EDITOR_PORT=8080`(에디터), 파일 서버 8081.

### 패널 기본 배치 변경 (24칸 그리드, `panels.tsx` slots)
- 윗줄: Pages 1~5 / Canvas 5~21 / Style Catalog 21~25. 아랫줄: Attributes 1~10 / Timeline·Playback 10~21(이전 10~20) / Color 21~25(이전 20~25).
  → Color 폭이 Style Catalog와 같아지고, Timeline 오른쪽 끝이 Canvas 오른쪽 끝과 맞음.
- Timeline 을 Canvas 와 완전히 같은 폭(16칸)으로 하려면 Attributes 를 4칸으로 줄여야 해서(내용 최소폭이 커 가로스크롤 발생) 보류.
- Color 패널: R/G/B/A 입력을 팔레트 옆 세로 열에서 팔레트 아래 가로 한 줄로 이동(`.channels`). 레이아웃은 localStorage 에 순서만 저장되므로 슬롯 변경은 즉시 반영.

### stop.sh: 8080 이 안 죽던 문제
- 원인 1: `bin/stop.sh`는 원래 cg-editor 를 일부러 남겨두게 돼 있었음 → 이제 마지막에 `src/cg-editor/stop.sh`도 호출해 함께 종료.
- 원인 2: `src/cg-editor/stop.sh`가 pid 파일에 의존했는데, `setsid` 때문에 파일 서버 pid 가 실제와 1 어긋나고(10707 vs 10706), 에디터는 리더(npm)만 먼저 죽고
  자식(`next-server`)이 남아 "이미 종료됨"으로 오판. → 그룹 TERM + 에디터/파일 서버 포트를 잡은 프로세스 직접 종료(TERM 후 KILL)로 변경.
- 종료 확인을 `fuser`가 아닌 `ss -ltn`으로 함: `fuser`는 다른 사용자(root) 프로세스를 못 봐서 root 서버가 살아 있는데도 "종료했습니다"라고 거짓 성공을 출력했음.
  못 죽이면 `sudo ./stop.sh` 안내와 함께 종료 코드 1.

### 교훈: root 로 띄운 서버/빌드의 소유권 충돌
- 터미널에서 root 로 `build`/`start`를 하면 `.next/`, `.run/`, `bin/.run/`이 root 소유가 되어 이후 pi 로 `build.sh`가 `EACCES`(`.next/trace`)로 실패하고, `bin/start.sh`도 pid 파일을 못 씀.
  해결: `sudo ./stop.sh` 후 `sudo chown -R pi:pi .next .run`, `bin/.run` 도 동일. 앞으로는 root 가 아닌 pi 로 `./start.sh` 사용.
- 빌드를 다시 하면 이미 떠 있는 서버(옛 빌드를 메모리에 가짐)는 청크 해시 불일치로 화면이 깨지므로 **빌드 후 반드시 서버 재시작**.

### 참고: Text Link
- Attributes 창의 Text Link = 선택한 텍스트 객체를 외부 `.txt` 와 연결. File System Access API 지원 브라우저는 2초마다 파일 변경을 감지해 자동 반영,
  미지원 브라우저는 선택 시점 1회만 읽음. 연결된 객체는 직접 편집 불가, `linkName`으로 cg-streamer `PUT /text/<linkName>` 갱신과도 연결됨.

### 재생 패널 탭 변경 (Timeline / Playback 창)
- 'Stream Control' 탭 이름을 **'Output Stream Control'**로 변경(`page.tsx`의 `playTab` 값과 탭 목록).
- 'Stamp Playback'·'Global Animation Playback' 탭은 **삭제하지 않고 탭 목록에서만 숨김**. 관련 코드(`channels.tsx`의 `aux.controls`, `page.tsx`의 분기)는 그대로 남아 있어서,
  다시 보이게 하려면 `page.tsx`의 탭 배열(`['Output Stream Control','Timeline','Playback','Run Setting', ...]`)에 두 이름을 되돌려 넣으면 됨(주석에 표시해 둠).
- 탭 순서: Output Stream Control · Timeline · Playback · Run Setting. 기본 선택 탭은 그대로 Timeline.

### Output Stream Control 에 "Output Project:" 라벨 추가
- 프로젝트 선택 콤보박스 **왼쪽**에 `Output Project:` 라벨 추가(`stream-control.tsx`, `.stream-label`). 처음엔 상태 표시줄의 프로젝트 이름 앞에 넣었다가
  요청으로 콤보 왼쪽으로 이동했고 상태 표시줄은 원래대로(`자막프로젝트 · Page 1/3 · …`).

### 단축키 변경: F5 → Ctrl+F5 (미리보기)
- 미리보기(처음부터 재생) 단축키를 F5 에서 **Ctrl+F5**(Mac 은 Cmd+F5)로 변경(`page.tsx` 키 핸들러 + 단축키 도움말 모달 문구). 처음엔 Shift+F5 로 바꿨다가 요청으로 Ctrl+F5 로 재변경.
- 단독 F5 는 앱이 가로채지 않아 브라우저 기본 동작(새로고침)이 됨. Ctrl+F5 는 브라우저의 강제 새로고침 단축키이지만 `preventDefault()` 로 막아 미리보기만 실행됨
  (입력창/텍스트 편집 중에는 핸들러가 무시되므로 그때는 브라우저 강제 새로고침이 동작).

### Run Setting 안내 문구 → 도움말로 이동
- Run Setting 탭 하단의 "Manual은 페이지 끝에서 멈춥니다. Skip으로 다음 페이지를 선택한 뒤 In을 누르세요." 문구를 삭제.
- 도움말 모달에는 "Manual은 페이지 끝에서 멈춥니다." 한 문장만 추가(Effects 안내 문단 뒤). Skip/In 사용 안내는 도움말에 넣지 않음.

### Style Catalog 견본 확장 (Color / Shape)
- **Color 탭**: 24색 → 약 150색(`page.tsx`의 `catalogColors`, 기존 `palettes`를 앞에 두고 중복 제거 후 이어 붙임). 참고 화면(Chyron 계열 Color 목록)의 초록/노랑/주황/빨강/분홍/보라/파랑/청록/갈색/어두운 계열 단색을 옮김.
  `palettes`는 Char/Shape 견본과 오른쪽 Color 창 팔레트도 쓰므로 건드리지 않고 Color 탭 전용 목록을 분리. 그리드 열 수 3열(넓은 화면 4열) → 6열(1650px↑ 8열).
- **Shape 탭**: 8개 → 약 130개(`catalogShapes`). 지금 모델이 `rect`/`ellipse`(색·테두리·투명도·그림자)만 지원해서 그걸로 만들 수 있는 것만 추가:
  굵은 띠(700×80)·중간 띠(28)·가는 선(6)·반투명 선(3), 원(300×300), 정사각/세로 사각형, 반투명, 흰 윤곽선, 글로우(그림자), 드롭섀도 변형 × 16색.
  견본 미리보기도 가로세로 비율·투명도·테두리·글로우를 반영하도록 개선(이전엔 항상 12px 막대).
- **도형 모델 확장 (그라데이션/줄무늬/둥근 사각형/테두리만/다각형)**: Item 에 선택 필드 4개 추가(`model.ts` `make()` 기본값 포함 → 저장/불러오기 때 보존, 기존 프로젝트는 기본값이라 영향 없음).
  - `radius`(모서리 반경, rect) · `shapeKind`(다각형 이름: triangle/wedge/quarter/parallelogram/trapezoid/slant/diamond/pentagon/hexagon/chevron/blob/leaf)
  - `gradient`: `linear:<각도>:<색1>:<색2>[:<색3>…]` 또는 `radial:0:<중심색>:<바깥색>`(각도는 CSS 와 같음: 0=위, 90=오른쪽, 180=아래; 색은 `#rrggbb[aa]` 만 허용)
  - `stripe`: `<각도>:<굵기px>:<색>` — 단색/그라데이션 위에 덧그리는 줄무늬
  - Framed Box 는 별도 필드 없이 `fill:'#00000000'`(투명)+`stroke`. 투명 채우기가 불러올 때 흰색으로 바뀌지 않도록 `normalizeProject`의 fill 검증을 8자리(#rrggbbaa)까지 허용.
  - 공용 그리기 코드 `app/shapes.ts`(`shapePath`/`drawShape`/`shapeCss`)를 만들어 에디터 캔버스(`editor-canvas.tsx`의 rect/ellipse 분기를 Shape 하나로 교체, 히트 영역도 같은 경로)와 썸네일/카탈로그 미리보기가 같이 씀.
  - **송출 쪽 `bin/web/cg-runtime.js`에도 같은 로직 이식**(`ITEM_DEFAULTS` 기본값, `SHAPE_POINTS`/`parseGradient`/`parseStripe`/`pathRounded`/`drawShape`). **shapes.ts 와 cg-runtime.js 는 한쪽을 고치면 다른 쪽도 같이 고칠 것.**
  - 헤드리스 Chromium 에서 런타임 코드로 둥근 사각형·원 그라데이션·줄무늬·테두리만·다각형(wedge/quarter/hexagon/chevron)·글로우가 그려지는 것 확인.
- **카탈로그**: Color 탭 끝에 그라데이션/줄무늬 견본 약 80개(위→아래·대각선·방사형·줄무늬·금속·가로 3색 등; 클릭하면 `fill`(대체 단색)+`gradient`+`stripe` 적용, 단색 견본을 누르면 gradient/stripe 초기화).
  Shape 탭은 Rounded Rect/Framed Box/그라데이션 막대·원/글로우/줄무늬 및 Custom 다각형 12종 × 단색·그라데이션 견본 추가(총 약 400개), 라벨도 Box/Circle/Rounded Rect/Framed Box/Custom 으로 표시.
- **한계**: 텍스트 객체는 `fill` 단색만 그리므로 그라데이션 견본을 텍스트에 누르면 대체 단색만 적용됨. Attributes 창에 radius/gradient/stripe/shapeKind 편집 UI 는 아직 없음(카탈로그로만 지정). 사각형 모서리 반경 외의 곡선 도형(Custom 곡선)은 다각형 근사.

### Attributes 창에 도형 편집 칸 추가
- rect/ellipse 선택 시 Attributes 탭 가운데 열 아래에 **Shape** 블록 표시(`attributes.tsx`의 `ShapeProps`): Radius(사각형, 다각형이 아닐 때) · Shape(None/다각형 12종) ·
  Gradient(None/Linear/Radial, Linear 각도, 색 2~3개 + 추가/제거 버튼) · Stripe(체크, 각도·굵기·Alpha %·색). 값은 `gradient`/`stripe` 문자열로 다시 조합해 저장(알파 있는 색은 알파 유지).
- 주의: 그라데이션이 켜져 있으면 `fill`(오른쪽 Color 창의 F)을 바꿔도 그라데이션이 우선이라 보이지 않음 → Gradient 를 None 으로 끄거나 Shape 블록의 색을 바꿔야 함.

### Shape 카탈로그 정리 (한눈에 보이게)
- 견본 수를 약 400 → 약 160개로 줄임(색 변형 축소, 중복 제거; Rounded Rect/Framed Box/그라데이션/Custom 종류는 모두 유지).
- 격자 표시: Shape 탭 전용 `.shape-stylegrid`(4열, 1650px↑ 5열, 칸 높이 56px 고정). 이전엔 정사각형 견본이 행 높이에 잡히지 않아 줄끼리 겹치고 라벨(Box/Circle/Rounded Rect/Framed Box/Custom)이 가려졌음.
  열이 패널 폭을 넘지 않도록 `minmax(0,1fr)` + 버튼 `min-width:0;overflow:hidden`.
- 긴 막대 견본은 칸 폭의 80%, 높이 4~20px 로 표시하고 정사각/세로 도형은 비율을 유지(`page.tsx`의 견본 미리보기).
- 헤드리스 Chromium(CDP)으로 실제 화면을 캡처해 확인하는 방법: `--remote-debugging-port` 로 띄우고 Shape 탭 클릭 후 `Page.captureScreenshot`.

### Style Catalog > Page 탭에 기본 템플릿 13종 추가 (문자발생기 자주 쓰는 구성)
- `app/templates.ts`(`builtinTemplates`)를 새로 만들고, Page 탭에 프로젝트 사용자 템플릿(`project.templates`) **앞에** 표시. 클릭하면 현재 페이지 내용이 템플릿으로 교체(기존 동작, 이름은 유지).
  하단 자막 1줄 / 하단 자막 2줄(제목+내용) / 인터뷰 이름표 / 뉴스 속보 / 하단 가로 스크롤(crawl) / 타이틀(중앙) / 로고+시계 / 스코어보드 /
  정보 박스(우측, 날씨) / 순위표 5위 / 자막방송(대사 2줄) / 장소+LIVE / 공지 박스(중앙). 띠는 wipe, 글자는 fade 인/아웃 기본 적용, 둥근 모서리(`radius`) 활용.
- 표시: 2열 격자(`.page-stylegrid`), 썸네일 높이 64px 고정(이전 Shape 탭과 같은 행 겹침 방지), 썸네일 글자 배율 `Thumb`의 `k` 매개변수(카탈로그에서는 0.5).
- 헤드리스 Chromium 캡처로 2열 격자·이름·썸네일 표시 확인.
- 운영 메모: 이 보드는 cg-streamer(송출 중 CPU ~200%) + 메모리 3.9GB 로 `tsc`가 3분 이상 걸리고 메모리 부족(exit 137)으로 에디터/파일 서버까지 같이 죽을 수 있음.
  타입체크/빌드는 송출을 멈춘 상태에서 하는 것이 안전. (`curl -X POST http://127.0.0.1:5555/quit` 로 cg-streamer 정지)
