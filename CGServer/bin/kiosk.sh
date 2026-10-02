#!/bin/bash
# 에디터 키오스크(Chromium)를 시작/종료/재시작한다. start.sh, stop.sh 와 display.sh(모니터 배치가 바뀌었을 때 재시작)가 사용.
#   ./kiosk.sh start [에디터포트]   이미 떠 있으면 그대로 둠
#   ./kiosk.sh stop
#   ./kiosk.sh restart              현재 에디터 모니터 위치/크기(.run/display-editor.geom)로 다시 띄움
set -u
cd "$(dirname "$(readlink -f "$0")")"
mkdir -p .run log
export DISPLAY=${DISPLAY:-:0}
CHROMIUM=/opt/chromium.org/stable/chromium-browser
KIOSK_PROFILE=/tmp/cg-editor-kiosk-$(id -un)   # 사용자별 폴더(root 로 만든 폴더가 남아 있으면 다른 계정이 못 써서 Chromium 이 안 뜸)
PIDF=.run/kiosk-chromium.pid
PORTF=.run/kiosk.port

alive() { [ -f "$PIDF" ] && kill -0 "$(cat "$PIDF")" 2>/dev/null; }
stop() {
  if [ -f "$PIDF" ]; then
    pid=$(cat "$PIDF")
    pkill -P "$pid" 2>/dev/null
    kill "$pid" 2>/dev/null
    rm -f "$PIDF"
  fi
}
start() {
  local port=${1:-}
  [ -n "$port" ] && echo "$port" > "$PORTF"
  port=$(cat "$PORTF" 2>/dev/null)
  [ -n "$port" ] || { echo "  에디터 포트를 알 수 없음 (./kiosk.sh start <포트>)"; return 1; }
  if alive; then echo "  이미 실행 중"; return 0; fi
  # 에디터 모니터(display.sh 가 기록한 .run/display-editor.geom "x y w h")에 띄운다. 모니터가 두 대면 에디터 쪽 모니터, 한 대면 그 모니터.
  local pos=()
  if read -r gx gy gw gh < .run/display-editor.geom 2>/dev/null && [ -n "${gh:-}" ]; then
    pos=(--window-position="$gx,$gy" --window-size="$gw,$gh")
  fi
  # 에디터는 큰 화면(1600x900 이상) 기준이라 작은 모니터(예: 1024x600)에서는 패널이 잘린다 -> 배율을 낮춰 더 넓은 화면처럼 보이게 한다.
  # cgsetup.cfg 의 editor_scale: auto(기본: 모니터가 1600x900 보다 작으면 그 비율로 축소) 또는 숫자(예: 0.64, 1 이면 축소 안 함)
  local scale
  scale=$(sed -n 's/#.*//; s/^[[:space:]]*editor_scale[[:space:]]*=[[:space:]]*\(.*[^[:space:]]\)[[:space:]]*$/\1/p' cgsetup.cfg 2>/dev/null | tail -1)
  if [ -z "$scale" ] || [ "$scale" = auto ]; then
    scale=$(awk -v w="${gw:-1600}" -v h="${gh:-900}" 'BEGIN{s=w/1600; t=h/900; if(t<s)s=t; if(s>1)s=1; printf "%.2f", s}')
  fi
  pos+=(--force-device-scale-factor="$scale")
  setsid nohup "$CHROMIUM" --kiosk "${pos[@]}" --noerrdialogs --disable-infobars --no-first-run --disable-features=Translate \
    --user-data-dir="$KIOSK_PROFILE" "http://localhost:$port" \
    > log/chromium-kiosk.log 2>&1 &
  echo $! > "$PIDF"
  disown
  echo "  시작됨 (pid $(cat "$PIDF"))"
}

case "${1:-}" in
  start) start "${2:-}" ;;
  stop) stop ;;
  restart) stop; sleep 1; start ;;
  *) echo "사용법: $0 start [포트] | stop | restart"; exit 2 ;;
esac
