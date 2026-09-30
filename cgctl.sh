#!/bin/bash
# cef_mpp 실행 중 제어 (SSH 로 보드에 접속한 상태에서 사용)
#   ./cgctl.sh next | prev | goto N | quit | status
# 명령은 UDP 127.0.0.1:5555 로 전달된다 (cef_mpp --run 이 수신).
PORT=${CG_CTL_PORT:-5555}
LOG=${CG_LOG:-$(dirname "$(readlink -f "$0")")/build/run_udp.log}

send() { printf '%s\n' "$1" | nc -u -w0 127.0.0.1 "$PORT" && echo "보냄: $1"; }

case "$1" in
  next|prev|quit) send "$1" ;;
  goto)
    [[ "$2" =~ ^[0-9]+$ ]] || { echo "사용법: $0 goto N (1부터)"; exit 1; }
    send "goto $2" ;;
  status)
    if pgrep -x cef_mpp >/dev/null; then
      echo "실행 중 (pid $(pgrep -o -x cef_mpp))"
      [ -r "$LOG" ] && { grep -E '^\[(cg|video|hdmirx)\]' "$LOG" | tail -3; grep '^\[stat\]' "$LOG" | tail -1; }
    else
      echo "실행 중 아님"
    fi ;;
  *) echo "사용법: $0 next | prev | goto N | quit | status"; exit 1 ;;
esac
