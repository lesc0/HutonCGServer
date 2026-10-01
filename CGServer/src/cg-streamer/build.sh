#!/bin/bash
# cg-streamer 빌드 → bin/cg-streamer (+ libcef.so, 리소스)
#   ./build.sh [release]  MinSizeRel + -O3 로 빌드 (기본)
#   ./build.sh debug      Debug 로 빌드 (-g -O0)
#   ./build.sh clean      빌드 디렉터리(build/rk3588-debug, build/rk3588-release) 삭제
#   ./build.sh install    release 로 빌드한 뒤 결과(실행 파일 + CEF 런타임)를 bin/ 으로 복사
#   --jobs N              동시 컴파일 수 (기본 2: 보드 메모리 3.9GB 에서 OOM 을 피하려고 낮춤, CG_BUILD_JOBS 로도 지정)
# CEF_ROOT 는 환경변수 > 보드 기본 경로(/opt/cef/cef_custom_130_arm64) 순으로 찾는다.
# 빌드 결과는 build/rk3588-<종류>/out 에 만들어지고(bin/ 은 install 때만 바뀜), 그 옆에 bin/web 링크가 있어 거기서 바로 실행해 볼 수 있다.
# 실행 중인 cg-streamer 가 있으면 install 한 새 실행 파일은 재시작해야 반영된다(./start.sh 또는 rebuild.sh).
set -e
cd "$(dirname "$(readlink -f "$0")")"

MODE=release; JOBS=${CG_BUILD_JOBS:-2}
while [ $# -gt 0 ]; do
  case "$1" in
    release|debug|clean|install) MODE=$1 ;;
    --jobs) shift; JOBS=${1:?--jobs 다음에 숫자가 필요합니다} ;;
    -h|--help) sed -n '2,10p' "$0"; exit 0 ;;
    *) echo "알 수 없는 인자: $1 (release|debug|clean|install)"; exit 1 ;;
  esac
  shift
done

if [ "$MODE" = clean ]; then
  echo "[build] 빌드 디렉터리 삭제: ../../build/rk3588-debug ../../build/rk3588-release"
  rm -rf ../../build/rk3588-debug ../../build/rk3588-release
  exit 0
fi

command -v cmake >/dev/null || { echo "cmake 가 없습니다 (3.21 이상 필요)"; exit 1; }
command -v ninja >/dev/null || { echo "ninja 가 없습니다 (sudo apt install ninja-build)"; exit 1; }
cmake --version | head -1 | awk '{split($3,v,"."); exit !(v[1]>3||(v[1]==3&&v[2]>=21))}' \
  || { echo "cmake 3.21 이상이 필요합니다 ($(cmake --version | head -1))"; exit 1; }

if [ -z "${CEF_ROOT:-}" ]; then
  CEF_ROOT=/opt/cef/cef_custom_130_arm64
fi
[ -d "$CEF_ROOT" ] || { echo "CEF_ROOT 를 찾을 수 없습니다: $CEF_ROOT (환경변수 CEF_ROOT 를 지정하세요)"; exit 1; }
export CEF_ROOT

# release/install: MinSizeRel 이지만 최적화는 -O3 (기본 -Os 를 덮어씀)
if [ "$MODE" = debug ]; then
  BUILD_DIR=../../build/rk3588-debug
  OPTS=(-DCMAKE_BUILD_TYPE=Debug)
else
  BUILD_DIR=../../build/rk3588-release
  OPTS=(-DCMAKE_BUILD_TYPE=MinSizeRel "-DCMAKE_C_FLAGS_MINSIZEREL=-O3 -DNDEBUG" "-DCMAKE_CXX_FLAGS_MINSIZEREL=-O3 -DNDEBUG")
fi

pgrep -x cg-streamer >/dev/null && echo "[build] 주의: cg-streamer 가 실행 중입니다. 빌드 후 재시작해야 반영됩니다."

echo "[build] cmake ($MODE, CEF_ROOT=$CEF_ROOT)"
OUT_DIR=$(mkdir -p "$BUILD_DIR/out" && cd "$BUILD_DIR/out" && pwd)
cmake --preset rk3588 -B "$BUILD_DIR" -DCG_OUT_DIR="$OUT_DIR" "${OPTS[@]}"
echo "[build] 빌드 (-j$JOBS)"
cmake --build "$BUILD_DIR" --parallel "$JOBS"
ln -sfn "$(cd ../../bin && pwd)/web" "$OUT_DIR/web"

if [ "$MODE" = install ]; then
  BIN_DIR=$(cd ../../bin && pwd)
  echo "[build] bin/ 으로 복사"
  find "$OUT_DIR" -mindepth 1 -maxdepth 1 ! -name web -exec cp -au -t "$BIN_DIR" {} +
fi
echo "[build] 완료: ../../bin/cg-streamer ($MODE)"
