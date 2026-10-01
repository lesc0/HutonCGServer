#!/bin/bash
# start.sh 로 띄운 것 종료: Chromium 키오스크 + cg-streamer + cg-editor.
set -u
cd "$(dirname "$(readlink -f "$0")")"
CHROMIUM_PIDF=.run/kiosk-chromium.pid

if [ -f "$CHROMIUM_PIDF" ]; then
  pid=$(cat "$CHROMIUM_PIDF")
  pkill -P "$pid" 2>/dev/null
  kill "$pid" 2>/dev/null
  rm -f "$CHROMIUM_PIDF"
fi

if pgrep -f "cg-streamer --run" >/dev/null; then
  curl -s -X POST http://127.0.0.1:5555/quit --max-time 3 >/dev/null
fi

# cg-editor 종료: 포트는 start.sh 와 같은 규칙(환경변수 > cgsetup.cfg editor_port > cg-editor/.env*)
CFG_PORT=$(sed -n 's/#.*//; s/^[[:space:]]*editor_port[[:space:]]*=[[:space:]]*\([0-9][0-9]*\).*/\1/p' cgsetup.cfg 2>/dev/null | tail -1)
(cd ../src/cg-editor && CG_EDITOR_PORT=${CG_EDITOR_PORT:-${CFG_PORT:-}} ./stop.sh)
