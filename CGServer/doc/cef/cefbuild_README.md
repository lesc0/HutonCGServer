# CEF 130 소스 빌드 (linuxarm64, v4l2 코덱 활성화)

x86 호스트에서 Chromium 130 + CEF를 **소스부터 크로스 빌드**(target_cpu=arm64)한 작업 디렉터리.
순정 prebuilt(`../src`, 삭제됨)는 HW 디코드가 안 돼서 `use_v4l2_codec=true` 로 직접 빌드했다.

- CEF: branch `6723`, commit `5a7e5ed` (`130.1.16+g5a7e5ed+chromium-130.0.6723.117`)
- Chromium: tag `130.0.6723.117` (`--no-history` 체크아웃)
- 최종 결과물: `../cef_custom_130_arm64.tar.gz` (libcef.so 약 223MB, strip 됨)
- 용량: 약 34GB (대부분 `dl/chromium`)

## 폴더
| 경로 | 내용 |
|---|---|
| `automate-git.py` | CEF 공식 빌드 스크립트 (python3 / github URL로 수정한 버전). `automate-git.master.py`는 원본 master 버전 |
| `depot_tools/` | Chromium depot_tools (gclient, ninja, gn) |
| `dl/cef/` | CEF git 소스 |
| `dl/chromium/src/` | Chromium 소스 + CEF 패치 적용본 (`src/cef`는 `dl/cef` 복사본) |
| `dl/chromium/src/out/Release_GN_arm64/` | ninja 빌드 출력 (`libcef.so`, `cefsimple` 등) |

## 빌드 설정 (`out/Release_GN_arm64/args.gn`)
```
is_official_build=true  is_debug=false  is_component_build=false
target_cpu="arm64"  use_sysroot=true  symbol_level=0
proprietary_codecs=true  ffmpeg_branding="Chrome"  enable_widevine=true
use_v4l2_codec=true  use_v4lplugin=true  use_vaapi=false
treat_warnings_as_errors=false      # 아래 에러 1 때문에 필요
use_partition_alloc_as_malloc=false  enable_backup_ref_ptr_support=false
is_cfi=false  use_thin_lto=false  chrome_pgo_phase=0
optimize_webui=true  enable_nacl=false  use_qt=false  clang_use_chrome_plugins=false
enable_linux_installer=false  enable_background_mode=false
enable_downgrade_processing=false  enable_resource_allowlist_generation=false
disable_fieldtrial_testing_config=true  forbid_non_component_debug_builds=false
```

## Rockchip(RK3588) 패치
`../patches-src/debian/patches/rkmpp/` 의 패치 20개(`series` 155~174행)를 Chromium 소스에 적용했다.
(Rockchip 엔지니어 Jeffy Chen, Jianfeng Liu 등 작성. 적용된 흔적: `dl/chromium/src`의 `media/gpu/v4l2`, `ui/gfx/linux` 등 변경.
어떤 명령으로 적용했는지는 기록이 없다.)
보드에는 libmali, v4l-rkmpp, mpp, 커스텀 libv4l2(mmap/munmap 지원)가 있어야 동작한다 (패치 0001/0004 설명).

| # | 패치 | 내용 |
|---|---|---|
| 0001 | HACK: media: Support V4L2 video decoder | V4L2(MPP) 비디오 디코더 지원 (v4l2_device/queue/utils, fourcc, `gpu_mojo_media_client_linux`) |
| 0002 | HACK: media: gpu: v4l2: Enable V4L2 VEA | V4L2 비디오 인코더(VEA) 활성화 |
| 0003 | media: gpu: v4l2: Gen libv4l2_stubs | libv4l2 stub 생성 (BUILD.gn) |
| 0004 | media: gpu: v4l2: Support libv4l2 plugins | libv4l2 플러그인 사용 (v4l2.sig, v4l2_device) |
| 0005 | media: capture: linux: Support libv4l2 plugins | V4L2 카메라 캡처에도 libv4l2 플러그인 허용 |
| 0006 | cld3: Avoid unaligned accesses | `port.h` 비정렬 접근으로 인한 SIGBUS(BUS_ADRALN) 회피 |
| 0007 | media: gpu: v4l2: Use POLLIN for pending event | v4l-rkmpp가 eventfd로 poll을 흉내내 POLLPRI 미지원 → POLLIN 사용 |
| 0008 | media: capture: linux: Prefer using the first device | 카메라 장치 배열을 뒤집어 첫 장치 우선 |
| 0009 | media: gpu: v4l2: Fix compile error when ozone not enabled | ozone 비활성 시 컴파일 에러 수정 |
| 0010 | Create new fence when there's no in-fences | in-fence가 없을 때 새 fence 생성 (wayland `gbm_surfaceless_wayland`) |
| 0011 | HACK: ozone/wayland: Force disable implicit external sync | Mali의 implicit external sync가 깨져 있어 강제 비활성화 |
| 0012 | HACK: media: capture: linux: Allow camera without supported frame sizes | discrete 프레임 크기만 허용하던 것을 완화 (Rockchip ISP 카메라용) |
| 0013 | content: gpu: Only depend dri for X11 | `content/gpu/BUILD.gn` dri 의존성을 X11일 때만 |
| 0014 | media: gpu: sandbox: Only depend dri for X11 | `media/gpu/sandbox/BUILD.gn` 동일 |
| 0015 | ui: gfx: linux: Force disabling modifiers | GBM modifier 사용 시 크래시 → `gbm_wrapper.cc`에서 `if (true \|\| modifiers.empty())`로 강제 비활성화 |
| 0016 | HACK: ui: x11: Fix config choosing error with Mali DDK | Mali DDK는 첫 번째 호환 visual의 EGL config만 보고 → X11에서 visual 선택 수정 (`gl_surface_egl_x11.cc`) |
| 0017 | enable widevine on arm64 linux | `widevine.gni`에서 arm64 Linux Widevine 활성화 |
| 0018 | ozone/wayland: revert implicit sync interop | wayland implicit sync interop 되돌림 (`client_native_pixmap_dmabuf.cc`, `dmabuf_uapi.h` 삭제 등) |
| 0019 | media: enable NV12 direct rendering | `gbm_wrapper.cc` NV12 직접 렌더링 |
| 0020 | media: v4l2: enable AV1 for stateful decoder | stateful 디코더에서 AV1 허용 |

GN 인자 `use_v4l2_codec=true`, `use_v4lplugin=true`, `use_vaapi=false`, `enable_widevine=true`가 이 패치들과 짝이다.

## GPU / HW 디코드 실행 옵션 (소스 기준 정리, 실행 검증 전)
확인한 것 (소스):
- `use_v4l2_codec=true` 로 빌드해서 HW 디코더가 컴파일 시점에 V4L2로 고정됨
  (`media/mojo/services/gpu_mojo_media_client_linux.cc`의 `GetPreferredLinuxDecoderImplementation()`이 V4L2 반환).
  `--enable-features=VaapiVideoDecoder` 같은 기능 플래그는 필요 없음.
- NV12 zero-copy 렌더링 경로는 `gr_context_type == kGL`일 때만 켜짐 → Vulkan이 아닌 GL 백엔드 사용.
- 패치 0016은 X11에서 Mali EGL config 선택을 고친 것 → `--ozone-platform=x11`, `wayland` 둘 다 의도된 경로.

시도해 볼 옵션 (추정):
```bash
./cefsimple --ozone-platform=x11 --use-gl=angle --use-angle=gles-egl \
            --ignore-gpu-blocklist --enable-gpu-rasterization --url=...
# Wayland: --ozone-platform=wayland
# /dev/video-dec0, dma-heap 권한 문제가 의심되면 --disable-gpu-sandbox 로 원인 분리
```
- `--ignore-gpu-blocklist`: Mali가 블록리스트에 걸릴 때 대비.
- 확인: `chrome://gpu`(cefclient 또는 `--remote-debugging-port`)에서 "Video Decode: Hardware accelerated".
- 보드 사전 조건: libmali, v4l-rkmpp, mpp, 커스텀 libv4l2, `/dev/video-dec0` 존재.

## 빌드 이력과 에러

### 1. 소스 받기 — (실패)
- `gclient sync --nohooks --no-history`로 Chromium을 받음. 서브모듈 `git fetch --depth=1`이 여러 번
  `failed; will retry after a short nap...` 경고를 내며 재시도됨(네트워크 불안정, 대부분 재시도로 통과).
- **에러**: 마지막에 CEF 패치/hook 단계에서 실패
  ```
  can't open file '.../src/cef/tools/version_manager.py': No such file or directory
  CalledProcessError: ... version_manager.py -a ... exit status 2
  ```
  원인: `automate-git.py`가 branch 6723(CEF 130)에는 없는 `tools/version_manager.py`(master 전용)를 호출.
  → master용 `automate-git.master.py`가 아닌, 이 브랜치에 맞는 버전의 `automate-git.py`를 쓰도록 교체
  (추정: 두 파일 diff: github URL, python3 전용 처리, 병렬 처리 등). 그 뒤 `gclient runhooks` 이후 단계 재실행.
- 이후 gn hook 단계: CEF 패치 103개 적용 확인 (`103 patches total (0 applied, 103 skipped, 0 failed)` = 이미 적용됨).

### 2. 1차 빌드 (실패, 20050/86040에서 중단)
```
FAILED: obj/ui/gfx/linux/gbm/gbm_wrapper.o
../../ui/gfx/linux/gbm_wrapper.cc:324:19: error: code will never be executed [-Werror,-Wunreachable-code]
    if (true || modifiers.empty()) {
```
원인: Rockchip 패치 0015(modifier 강제 비활성화)가 넣은 `true ||`가 `-Wunreachable-code`에 걸림.
해결: GN 인자에 `treat_warnings_as_errors=false` 추가 후 재개.

### 3. 2차 빌드 (83545 스텝, 거의 끝에서 실패)
- 라이브러리/`libcef.so`까지는 정상 빌드됨. 실패한 것은 테스트 앱 `cefclient`/`ceftests` 뿐:
  ```
  fatal error: 'gtk/gtk.h' file not found
  fatal error: 'gdk/gdk.h' file not found
  ```
  원인: sysroot(Debian)에 GTK3 개발 헤더가 없음. cefclient/ceftests는 GTK에 의존.
- 해결(로그 기준 추정): `cefclient`/`ceftests` 빌드는 포기하고 `cefsimple`만 타깃으로 지정(GTK 불필요).

### 4. 3차 빌드 (성공)
```
ninja -C out/Release_GN_arm64 cefsimple
[1/3] AR obj/cef/libcef_dll_wrapper.a
[2/3] SOLINK ./libcef.so
[3/3] LINK ./cefsimple
```
이후 `libcef.so`를 strip(2GB+ → 약 223MB)하고 헤더/`libcef_dll`/Release/Resources를 묶어
`../cef_custom_130_arm64.tar.gz` 생성.

## 다시 빌드할 때
```bash
cd dl/chromium/src
export PATH=$PWD/../../../depot_tools:$PATH DEPOT_TOOLS_UPDATE=0
# args.gn이 이미 있으므로 gn 재생성 생략 가능
autoninja -C out/Release_GN_arm64 cefsimple      # cefclient/ceftests는 GTK 헤더 없어서 실패
```
처음부터 하려면 `python3 automate-git.py --download-dir=dl --depot-tools-dir=depot_tools --branch=6723 --no-debug-build --arm64-build ...`
(위 args.gn 값은 `GN_DEFINES`로 전달. 실제 사용한 명령행은 기록이 없어 위 옵션은 추정.)

## 메모
- 이전 시도 중 1차/2차 빌드 사이에 `args.gn`이 바뀜 (warning-as-error 해제).
- 로그 파일은 정리하며 삭제함. 빌드 시각: 1차 9/30 14:52, 2차 9/30 18:47, 3차 10/1 01:21 종료.

## 보드에서 실행 (이전 README에서 옮김, 커스텀 빌드로는 미검증)
```bash
sudo chown root:root chrome-sandbox && sudo chmod 4755 chrome-sandbox   # 아니면 --no-sandbox
./cefsimple --ozone-platform=x11 --use-gl=angle --use-angle=gles-egl --url=https://www.google.com
```
필요 런타임 라이브러리: libgtk-3-0, libnss3, libasound2, libgbm1, libcups2, libxkbcommon0 (`ldd libcef.so | grep "not found"`로 확인).
