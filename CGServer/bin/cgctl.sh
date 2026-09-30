#!/bin/bash
# cg-streamer 실행 중 제어 (SSH 로 보드에 접속한 상태에서 사용)
#   ./cgctl.sh play [N] | run | stop | clear | pause | cut | skip | next | prev | goto N
#   ./cgctl.sh stamp 1|2 play|stop|pause | global play|stop
#   ./cgctl.sh text <링크이름> <문자열> | status | quit
# 명령은 HTTP 127.0.0.1:5555 로 전달된다 (cg-streamer --run 이 수신, --http/--bind/--token 으로 변경).
#   CG_HOST, CG_CTL_PORT, CG_TOKEN 환경변수로 대상/토큰 지정.
HOST=${CG_HOST:-127.0.0.1}
PORT=${CG_CTL_PORT:-5555}
URL="http://$HOST:$PORT"
AUTH=()
[ -n "$CG_TOKEN" ] && AUTH=(-H "Authorization: Bearer $CG_TOKEN")
LOG=${CG_LOG:-$(dirname "$(readlink -f "$0")")/../build/run_udp.log}

post() { curl -s -X "${2:-POST}" "${AUTH[@]}" "$URL$1" "${@:3}"; echo; }

case "$1" in
  play)
    if [ -n "$2" ]; then [[ "$2" =~ ^[0-9]+$ ]] || { echo "사용법: $0 play [N]"; exit 1; }; post "/play/$2"; else post /play; fi ;;
  run|stop|clear|pause|cut|skip|next|prev|quit) post "/$1" ;;
  goto)
    [[ "$2" =~ ^[0-9]+$ ]] || { echo "사용법: $0 goto N (1부터)"; exit 1; }
    post "/goto/$2" ;;
  stamp)
    [[ "$2" =~ ^[12]$ && "$3" =~ ^(play|stop|pause)$ ]] || { echo "사용법: $0 stamp 1|2 play|stop|pause"; exit 1; }
    post "/stamp/$2/$3" ;;
  global)
    [[ "$2" =~ ^(play|stop)$ ]] || { echo "사용법: $0 global play|stop"; exit 1; }
    post "/global/$2" ;;
  text)
    [ -n "$2" ] && [ -n "$3" ] || { echo "사용법: $0 text <링크이름> <문자열>"; exit 1; }
    # JSON 이스케이프는 python3 로 (없으면 큰따옴표/역슬래시가 없는 문자열만 안전)
    body=$(python3 -c 'import json,sys;print(json.dumps({"text":sys.argv[1]},ensure_ascii=False))' "$3" 2>/dev/null || printf '{"text":"%s"}' "$3")
    post "/text/$2" PUT -H 'Content-Type: application/json' --data-binary "$body" ;;
  status)
    if pgrep -x cg-streamer >/dev/null; then
      echo "실행 중 (pid $(pgrep -o -x cg-streamer))"
      curl -s "${AUTH[@]}" "$URL/status"; echo
      [ -r "$LOG" ] && { grep -E '^\[(cg|video|hdmirx)\]' "$LOG" | tail -3; grep '^\[stat\]' "$LOG" | tail -1; }
    else
      echo "실행 중 아님"
    fi ;;
  *) echo "사용법: $0 play [N] | run | stop | clear | pause | cut | skip | next | prev | goto N | stamp N act | global act | text 이름 문자열 | status | quit"; exit 1 ;;
esac
