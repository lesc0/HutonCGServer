# CEF 130 + Rockchip MPP 디코딩 패치 — 서버 빌드 가이드

목표: 공식 CEF 바이너리에 없는 **H.264 디코더 + V4L2(libv4l-rkmpp → MPP) 하드웨어 디코딩**을 넣은
CEF 130 linuxarm64를 x86_64 서버에서 크로스 빌드하고, 결과물만 CM3588 보드로 가져와 cef_mpp를 다시 빌드한다.

> ⚠️ 공개 자료를 바탕으로 정리한 절차이며 **아직 실제로 돌려 보지 않았다.** 특히 2단계(패치 적용)의 충돌 여부는
> 서버에서 확인해야 한다. 결과는 [../history.md](../history.md)에 기록할 것.

## 배경

| | 공식 CEF 130.1.16 (현재 `/opt/cef`) | FriendlyElec Chromium 130 (보드 시스템 브라우저) |
|---|---|---|
| mp4(H.264) `<video>` | 재생 불가(검은 화면) | 재생, MPP 하드웨어 디코딩 |
| V4L2 디코더 | 없음 (`/dev/video-dec`, `libv4l2.so` 문자열 0개) | 있음 |

- 보드(CM3588, 메모리 3.8GB, 디스크 여유 40GB)에서는 Chromium 소스 빌드가 불가능 → 서버에서 빌드.
- 같은 계열 공개 패치: [amazingfate/chromium-debian-build `rockchip-rkmpp-130`](https://github.com/amazingfate/chromium-debian-build/tree/rockchip-rkmpp-130)
  (`debian/patches/rkmpp/` 20개, Chromium 130.0.6723.116 기준). 원 출처: [JeffyCN/meta-rockchip](https://github.com/JeffyCN/meta-rockchip).
- 우리 CEF 130.1.16 = Chromium **130.0.6723.117** → 패치 기준 버전과 거의 동일.

## 서버 요구사항

- x86_64 Linux (Ubuntu 22.04 권장)
- 디스크 **150GB 이상**, 메모리 **32GB 이상**
- arm64 크로스 빌드 (sysroot 사용)

## 1. CEF 130 소스 받기 (브랜치 6723)

```bash
mkdir -p ~/code && cd ~/code
curl -O https://raw.githubusercontent.com/chromiumembedded/cef/6723/tools/automate/automate-git.py
python3 automate-git.py --download-dir=$HOME/code/chromium_git \
  --depot-tools-dir=$HOME/code/depot_tools --branch=6723 --no-build --no-distrib

cd ~/code/chromium_git/chromium/src
./build/install-build-deps.sh --arm
./build/linux/sysroot_scripts/install-sysroot.py --arch=arm64
```

## 2. rkmpp 패치 적용

```bash
cd ~/code
git clone -b rockchip-rkmpp-130 --depth 1 https://github.com/amazingfate/chromium-debian-build.git
cd ~/code/chromium_git/chromium/src
for p in ~/code/chromium-debian-build/debian/patches/rkmpp/*.patch; do
  echo "== $p"; patch -p1 --dry-run < "$p" && patch -p1 < "$p"
done
```

주요 패치:

| 패치 | 내용 |
|---|---|
| 0001 HACK-media-Support-V4L2-video-decoder | V4L2 디코더 활성화 (핵심) |
| 0003 Gen-libv4l2_stubs / 0004 Support-libv4l2-plugins | libv4l2 플러그인(libv4l-rkmpp) 로드 |
| 0016 HACK-ui-x11-Fix-config-choosing-error-with-Mali-DDK | Mali 블롭 + X11 EGL 설정 |
| 0019 media-enable-NV12-direct-rendering | NV12 직접 렌더링 |
| 0011, 0018 | Wayland 관련 — X11만 쓰면 불필요할 수 있음 |

- `--dry-run` 실패 패치는 건너뛰고 목록을 기록한다.

## 3. 빌드 (arm64, minimal 배포판)

```bash
export GN_DEFINES="is_official_build=true use_sysroot=true symbol_level=1 is_cfi=false chrome_pgo_phase=0 \
use_v4l2_codec=true use_v4lplugin=true use_vaapi=false proprietary_codecs=true ffmpeg_branding=Chrome"
export CEF_ARCHIVE_FORMAT=tar.bz2
cd ~/code
python3 automate-git.py --download-dir=$HOME/code/chromium_git \
  --depot-tools-dir=$HOME/code/depot_tools --branch=6723 \
  --no-update --force-build --arm64-build --no-debug-build --minimal-distrib
```

- `--no-update`: 2단계에서 적용한 패치가 덮어써지지 않게 함.
- GN 옵션 출처: amazingfate `debian/rules` (arm64: `use_v4l2_codec=true use_vaapi=false use_v4lplugin=true`,
  공통: `proprietary_codecs=true ffmpeg_branding="Chrome"`).
- 결과물: `chromium_git/chromium/src/cef/binary_distrib/cef_binary_130.*_linuxarm64_minimal.tar.bz2`

## 4. 보드로 가져와서 cef_mpp 재빌드

```bash
sudo tar xjf cef_binary_*_linuxarm64_minimal.tar.bz2 -C /opt/cef
export CEF_ROOT=/opt/cef/<새 폴더>          # ~/.bashrc 의 CEF_ROOT 도 수정
cd /root/work/CGServer && rm -rf build/rk3588
cmake --preset rk3588 && cmake --build --preset rk3588
```

## 5. 확인

1. V4L2 코드 포함 여부 (0보다 크면 포함):
   ```bash
   strings $CEF_ROOT/Release/libcef.so | grep -c /dev/video-dec
   ```
2. mp4 `<video>`를 **GPU 모드**로 실행 → GPU 프로세스가 `/dev/mpp_service`를 열면 성공.
   하드웨어 디코딩은 GPU 프로세스에서 일어나므로 `--gpu`가 필요하다.
   ```bash
   cd build/rk3588/Release
   CHROMIUM_USE_VDA=true ./cef_mpp --run --gpu --cef:use-gl=angle --cef:use-angle=gles-egl \
     --cef:ozone-platform=headless --cef:enable-accelerated-video-decode \
     --project=<mp4 video 페이지 project.json> --out=test.ts --seconds=10
   for p in $(pgrep -x cef_mpp); do tr '\0' ' ' </proc/$p/cmdline | grep -q gpu-process && ls -la /proc/$p/fd | grep -E 'mpp_service|mali0'; done
   ```
3. `CHROMIUM_USE_VDA=true`, `--enable-accelerated-video-decode`는 FriendlyElec 설정
   (`/etc/chromium.org/03-nanopi6`)과 같은 조건. 패치에 따라 필요 여부가 다를 수 있음.

## 알아둘 점

- 패치가 CEF 자체 패치와 충돌할 수 있다 — 가장 불확실한 단계.
- 보드는 Debian 11 (glibc 2.31). Chromium 130 sysroot는 bullseye 기준으로 알려져 있어 문제없을 것으로 보지만,
  실행해서 확인해야 한다.
- 현재 공식 CEF에 적용해 둔 cef_mpp CMakeLists 수정(`C CXX`, `PROJECT_ARCH=arm64`, `libcef_lib` 링크)은
  새 CEF에서도 그대로 필요하다.
