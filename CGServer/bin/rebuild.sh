#!/bin/bash
# cg-editor 다시 빌드: 메모리(3.9GB)가 모자라 tsc/빌드가 OOM 으로 죽는 것을 막기 위해
# 송출(cg-streamer)·키오스크·에디터를 먼저 내리고 빌드한 뒤, 원래 켜져 있던 것만 다시 시작한다.
#   ./rebuild.sh            빌드 후 재시작
#   ./rebuild.sh --check    빌드 전에 타입 검사(tsc --noEmit)도 실행 (메모리를 많이 씀)
set -u
cd "$(dirname "$(readlink -f "$0")")"

STREAMER=0; KIOSK=0
pgrep -f "cg-stream[e]r --run" >/dev/null && STREAMER=1
[ -f .run/kiosk-chromium.pid ] && kill -0 "$(cat .run/kiosk-chromium.pid)" 2>/dev/null && KIOSK=1
echo "[rebuild] 실행 중이던 것: cg-streamer=$STREAMER, 키오스크=$KIOSK"

echo "[rebuild] 정지"
./stop.sh
sleep 2

echo "[rebuild] 빌드"
(cd ../src/cg-editor && ./build.sh "$@")
RC=$?
[ "$RC" = 0 ] || echo "[rebuild] 빌드 실패(rc=$RC) - 이전 빌드 결과로 다시 시작합니다"

echo "[rebuild] 재시작"
CG_SKIP_STREAMER=$((1 - STREAMER)) CG_SKIP_KIOSK=$((1 - KIOSK)) ./start.sh
exit $RC
