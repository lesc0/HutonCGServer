#!/bin/bash
# cg-streamer 빌드: cmake 설정(최초 1회) + 빌드 → bin/cg-streamer (+ libcef.so, 리소스)
#   ./build.sh            rk3588 프리셋(기본 경로)으로 빌드
#   ./build.sh --accel    rk3588-accel 프리셋(실험: dmabuf 제로카피, --accel 로 실행)
#   ./build.sh --clean    빌드 디렉터리를 지우고 처음부터 다시 설정·빌드
#   ./build.sh --jobs N   동시 컴파일 수 (기본 2: 보드 메모리 3.9GB 에서 OOM 을 피하려고 낮춤)
# CEF_ROOT 는 환경변수 > 보드 기본 경로(/opt/cef/cef_custom_130_arm64) 순으로 찾는다.
# 실행 중인 cg-streamer 가 있으면 새 실행 파일은 재시작해야 반영된다(./start.sh 또는 rebuild.sh).
set -e
cd "$(dirname "$(readlink -f "$0")")"

PRESET=rk3588; CLEAN=0; JOBS=${CG_BUILD_JOBS:-2}
while [ $# -gt 0 ]; do
  case "$1" in
    --accel) PRESET=rk3588-accel ;;
    --clean) CLEAN=1 ;;
    --jobs) shift; JOBS=${1:?--jobs 다음에 숫자가 필요합니다} ;;
    -h|--help) sed -n '2,8p' "$0"; exit 0 ;;
    *) echo "알 수 없는 옵션: $1"; exit 1 ;;
  esac
  shift
done

command -v cmake >/dev/null || { echo "cmake 가 없습니다 (3.21 이상 필요)"; exit 1; }
command -v ninja >/dev/null || { echo "ninja 가 없습니다 (sudo apt install ninja-build)"; exit 1; }
cmake --version | head -1 | awk '{split($3,v,"."); exit !(v[1]>3||(v[1]==3&&v[2]>=21))}' \
  || { echo "cmake 3.21 이상이 필요합니다 ($(cmake --version | head -1))"; exit 1; }

if [ -z "${CEF_ROOT:-}" ]; then
  CEF_ROOT=/opt/cef/cef_custom_130_arm64
fi
[ -d "$CEF_ROOT" ] || { echo "CEF_ROOT 를 찾을 수 없습니다: $CEF_ROOT (환경변수 CEF_ROOT 를 지정하세요)"; exit 1; }
export CEF_ROOT

BUILD_DIR=../../build/$PRESET
if [ "$CLEAN" = 1 ]; then
  echo "[build] 빌드 디렉터리 삭제: $BUILD_DIR"
  rm -rf "$BUILD_DIR"
fi

pgrep -x cg-streamer >/dev/null && echo "[build] 주의: cg-streamer 가 실행 중입니다. 빌드 후 재시작해야 반영됩니다."

echo "[build] cmake --preset $PRESET (CEF_ROOT=$CEF_ROOT)"
cmake --preset "$PRESET"
echo "[build] cmake --build --preset $PRESET (-j$JOBS)"
cmake --build --preset "$PRESET" --parallel "$JOBS"
echo "[build] 완료: ../../bin/cg-streamer"
