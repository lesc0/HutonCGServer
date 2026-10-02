#!/bin/bash
# 로컬 화면(HDMI) 해상도 자동 설정: 연결된 첫 출력의 권장(첫) 모드를 적용하고, 연결 안 된 출력은 끄고, X 화면 크기도 같게 맞춘다.
# start.sh 가 스트리머를 띄우기 전에 부르고, 실행 중인 cg-streamer 도 reload/Switch project/POST /display 때 부른다(모니터를 바꿔 꽂은 경우 반영).
# 종료 코드는 항상 0 (적용할 출력이 없어도 오류로 취급하지 않음).
export DISPLAY=${DISPLAY:-:0}
command -v xrandr >/dev/null 2>&1 || exit 0

out=$(xrandr --query 2>/dev/null | awk '/ connected/{print $1; exit}')
[ -n "$out" ] || exit 0
best=$(xrandr --query 2>/dev/null | awk -v o="$out" 'f&&/^[^ ]/{f=0} $1==o{f=1;next} f{print $1;exit}')
[ -n "$best" ] || exit 0

xrandr --query 2>/dev/null | awk '/ disconnected/{print $1}' | while read -r o; do xrandr --output "$o" --off 2>/dev/null; done
# 화면이 커지는 경우는 fb 를 먼저, 작아지는 경우는 모드를 먼저 바꿔야 해서 순서대로 시도한다(실패는 무시).
xrandr --fb "$best" 2>/dev/null
xrandr --output "$out" --mode "$best" 2>/dev/null
xrandr --fb "$best" 2>/dev/null
exit 0
