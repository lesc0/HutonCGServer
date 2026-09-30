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
