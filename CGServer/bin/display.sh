#!/bin/bash
# 로컬 모니터 배치: **송출 모니터(output_display, 기본 HDMI-2)만** 제어한다. 다른 모니터(예: HDMI-1 데스크탑)는 모드/위치/켜짐 어느 것도 건드리지 않는다.
#  - 송출 모니터가 이미 켜져 있고 다른 모니터와 겹치지 않으면 아무것도 바꾸지 않고 그 영역만 읽어 기록한다.
#  - 꺼져 있거나(연결만 된 상태) 다른 모니터와 겹쳐 있으면(모니터를 꽂을 때 X 가 모두 0,0 에 겹쳐 놓음) 송출 모니터만 권장 모드로 켜서
#    다른 모니터들의 오른쪽에 둔다. 이때 화면(X)이 모자라면 크기만 키운다(줄이지 않음).
# 결과 영역은 .run/display-output.geom ("x y w h")에 기록 -> cg-streamer 가 미리보기 창 위치/크기로 읽는다.
# .run/display-editor.geom 은 에디터 모니터(editor_display)의 현재 영역(읽기만 함)이며 키오스크를 켰을 때만 쓰인다.
# start.sh 가 스트리머를 띄우기 전에 부르고, 실행 중인 cg-streamer 도 reload/Switch project/POST /display/모니터 연결 변경 때 부른다.
# 종료 코드는 항상 0.
export DISPLAY=${DISPLAY:-:0}
command -v xrandr >/dev/null 2>&1 || exit 0
HERE="$(dirname "$(readlink -f "$0")")"
RUN="$HERE/.run"; mkdir -p "$RUN"
CFG="$HERE/cgsetup.cfg"
cfgval() { sed -n "s/#.*//; s/^[[:space:]]*$1[[:space:]]*=[[:space:]]*\(.*[^[:space:]]\)[[:space:]]*$/\1/p" "$CFG" 2>/dev/null | tail -1; }
EDITOR_OUT=$(cfgval editor_display); EDITOR_OUT=${EDITOR_OUT:-HDMI-1}
OUTPUT_OUT=$(cfgval output_display); OUTPUT_OUT=${OUTPUT_OUT:-HDMI-2}

# 켜져 있는 모니터: "이름 x y w h" 한 줄씩. xrandr --listmonitors 한 줄 예: " 1: +HDMI-2 1024/271x600/159+1920+0  HDMI-2"
# (mawk 에서도 되도록 split 만 사용)
mons() {
  xrandr --listmonitors 2>/dev/null | awk 'NR>1 {
    n = $NF; s = $3
    k = split(s, p, "+"); if (k < 3) next
    split(p[1], d, "x"); split(d[1], a, "/"); split(d[2], b, "/")
    print n, p[2], p[3], a[1], b[1]
  }'
}
MONS=$(mons)
geom_of() { echo "$MONS" | awk -v o="$1" '$1==o{print $2,$3,$4,$5;exit}'; }
connected() { xrandr --query 2>/dev/null | awk -v o="$1" '$1==o && $2=="connected"{f=1} END{exit !f}'; }
# 권장 모드: 모드 줄에서 '+' 표시가 붙은 것(예: 60.00*+). 없으면 첫 번째 모드. (첫 줄이 권장이 아닌 모니터가 있음: 4:3 모니터 등)
pref_mode() { xrandr --query 2>/dev/null | awk -v o="$1" '$1==o{f=1;next} f&&/^[[:space:]]+[0-9]+x[0-9]+/{m=$1;sub(/[^0-9x].*$/,"",m); if(first=="")first=m; if($0 ~ /+/){print m; done=1; exit} next} f&&/^[^[:space:]]/{exit} END{if(!done&&first!="")print first}'; }

if connected "$OUTPUT_OUT"; then
  OG=$(geom_of "$OUTPUT_OUT")
  # 송출 모니터가 켜져 있지 않거나 다른 켜진 모니터와 겹치는지 확인
  NEED=0
  if [ -z "$OG" ]; then NEED=1; else
    read -r ox oy ow oh <<<"$OG"
    while read -r n x y w h; do
      { [ -z "$n" ] || [ "$n" = "$OUTPUT_OUT" ]; } && continue
      if [ "$ox" -lt $((x + w)) ] && [ $((ox + ow)) -gt "$x" ] && [ "$oy" -lt $((y + h)) ] && [ $((oy + oh)) -gt "$y" ]; then NEED=1; fi
    done <<<"$MONS"
  fi
  if [ "$NEED" = 1 ]; then
    M=$(pref_mode "$OUTPUT_OUT"); [ -n "$M" ] || M=1920x1080
    W=${M%x*}; H=${M#*x}
    # 다른 켜진 모니터들의 오른쪽 끝(없으면 0)과 아래쪽 끝
    RX=0; RH=0
    while read -r n x y w h; do
      { [ -z "$n" ] || [ "$n" = "$OUTPUT_OUT" ]; } && continue
      [ $((x + w)) -gt "$RX" ] && RX=$((x + w))
      [ $((y + h)) -gt "$RH" ] && RH=$((y + h))
    done <<<"$MONS"
    FW=$((RX + W)); FH=$((RH > H ? RH : H))
    read -r CW CH <<<"$(xrandr --query 2>/dev/null | sed -n 's/^Screen 0:.*current \([0-9]*\) x \([0-9]*\).*/\1 \2/p' | head -1)"
    # 화면은 키우기만 한다(다른 모니터를 건드리지 않기 위해 줄이지 않음)
    if [ "${CW:-0}" -lt "$FW" ] || [ "${CH:-0}" -lt "$FH" ]; then
      xrandr --fb "$((CW > FW ? CW : FW))x$((CH > FH ? CH : FH))" 2>/dev/null
    fi
    xrandr --output "$OUTPUT_OUT" --mode "$M" --pos "${RX}x0" 2>/dev/null
    MONS=$(mons)
  fi
  OG=$(geom_of "$OUTPUT_OUT")
  [ -n "$OG" ] && echo "$OG" > "$RUN/display-output.geom"
else
  rm -f "$RUN/display-output.geom"   # 송출 모니터가 없으면 기록을 지워 cg-streamer 가 화면 전체를 쓰게 함(아무 것도 제어하지 않음)
fi

# 에디터 모니터의 현재 영역(읽기만): 없으면 송출 모니터와 같은 곳
EG=$(geom_of "$EDITOR_OUT"); [ -n "$EG" ] || EG=$(cat "$RUN/display-output.geom" 2>/dev/null)
[ -n "$EG" ] && echo "$EG" > "$RUN/display-editor.geom"
exit 0
