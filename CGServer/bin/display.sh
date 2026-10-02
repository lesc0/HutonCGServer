#!/bin/bash
# 로컬 화면(HDMI) 자동 설정.
#  - 모니터가 두 대 연결되어 있으면: editor_display(기본 HDMI-1)를 왼쪽에 에디터(Chromium 키오스크), output_display(기본 HDMI-2)를 그 오른쪽에 송출 화면(cg-streamer 미리보기)으로 나란히 배치
#  - 한 대만 연결되어 있으면: 그 모니터의 권장 모드로 맞추고 에디터와 송출 화면이 같은 모니터를 쓴다
# 각 출력의 권장(첫) 모드를 쓰고, 연결 안 된 출력은 끄고, X 화면 크기도 맞춘다.
# 결과 위치는 .run/display-editor.geom / .run/display-output.geom ("x y w h")에 기록 -> start.sh(키오스크 위치)와 cg-streamer(미리보기 창 위치/크기)가 읽는다.
# start.sh 가 스트리머를 띄우기 전에 부르고, 실행 중인 cg-streamer 도 reload/Switch project/POST /display 때 부른다(모니터를 바꿔 꽂은 경우 반영).
# 종료 코드는 항상 0 (적용할 출력이 없어도 오류로 취급하지 않음).
export DISPLAY=${DISPLAY:-:0}
command -v xrandr >/dev/null 2>&1 || exit 0
HERE="$(dirname "$(readlink -f "$0")")"
RUN="$HERE/.run"; mkdir -p "$RUN"
CFG="$HERE/cgsetup.cfg"
cfgval() { sed -n "s/#.*//; s/^[[:space:]]*$1[[:space:]]*=[[:space:]]*\(.*[^[:space:]]\)[[:space:]]*$/\1/p" "$CFG" 2>/dev/null | tail -1; }
EDITOR_OUT=$(cfgval editor_display); EDITOR_OUT=${EDITOR_OUT:-HDMI-1}
OUTPUT_OUT=$(cfgval output_display); OUTPUT_OUT=${OUTPUT_OUT:-HDMI-2}

# 연결된 출력과 권장(첫) 모드: "이름 모드" 한 줄씩
CONN=$(xrandr --query 2>/dev/null | awk '/ connected/{o=$1;f=1;next} f&&/^[[:space:]]+[0-9]+x[0-9]+/{m=$1;sub(/[^0-9x].*$/,"",m);print o,m;f=0}')
[ -n "$CONN" ] || exit 0
modeof() { echo "$CONN" | awk -v o="$1" '$1==o{print $2;exit}'; }   # 출력 이름 -> 모드(없으면 빈 값)

# 연결 안 된 출력은 끈다
xrandr --query 2>/dev/null | awk '/ disconnected/{print $1}' | while read -r o; do xrandr --output "$o" --off 2>/dev/null; done

OLD_EDITOR_GEOM=$(cat "$RUN/display-editor.geom" 2>/dev/null)
ME=$(modeof "$EDITOR_OUT"); MO=$(modeof "$OUTPUT_OUT")
if [ -n "$ME" ] && [ -n "$MO" ] && [ "$EDITOR_OUT" != "$OUTPUT_OUT" ]; then
  # ---- 모니터 두 대: 에디터(왼쪽) + 송출(오른쪽) ----
  W1=${ME%x*}; H1=${ME#*x}; W2=${MO%x*}; H2=${MO#*x}
  FW=$((W1 + W2)); FH=$((H1 > H2 ? H1 : H2))
  xrandr --fb "${FW}x${FH}" 2>/dev/null     # 화면이 커지므로 먼저 크게 만든 뒤 배치
  xrandr --output "$EDITOR_OUT" --mode "$ME" --pos 0x0 --output "$OUTPUT_OUT" --mode "$MO" --pos "${W1}x0" 2>/dev/null
  xrandr --fb "${FW}x${FH}" 2>/dev/null
  echo "0 0 $W1 $H1" > "$RUN/display-editor.geom"
  echo "$W1 0 $W2 $H2" > "$RUN/display-output.geom"
else
  # ---- 한 대: 첫 번째 연결된 출력을 에디터/송출 겸용으로 ----
  OUT=$(echo "$CONN" | awk 'NR==1{print $1}'); M=$(modeof "$OUT")
  W=${M%x*}; H=${M#*x}
  xrandr --fb "${W}x${H}" 2>/dev/null
  xrandr --output "$OUT" --mode "$M" --pos 0x0 2>/dev/null
  xrandr --fb "${W}x${H}" 2>/dev/null
  echo "0 0 $W $H" > "$RUN/display-editor.geom"
  echo "0 0 $W $H" > "$RUN/display-output.geom"
fi
# 에디터 모니터의 위치/크기가 바뀌었고 키오스크가 떠 있으면 새 위치로 다시 띄운다(모니터를 뽑았다 꽂은 경우). 처음 시작할 때는 start.sh 가 띄우므로 건드리지 않음.
NEW_EDITOR_GEOM=$(cat "$RUN/display-editor.geom" 2>/dev/null)
if [ -n "$OLD_EDITOR_GEOM" ] && [ "$OLD_EDITOR_GEOM" != "$NEW_EDITOR_GEOM" ] && [ -f "$RUN/kiosk-chromium.pid" ] && kill -0 "$(cat "$RUN/kiosk-chromium.pid")" 2>/dev/null; then
  ( setsid bash "$HERE/kiosk.sh" restart >/dev/null 2>&1 & )
fi
exit 0
