#!/bin/bash
# cg-editor 시작 (백그라운드 실행, PID/로그는 .run/ 에 저장)
#   ./start.sh                프로덕션 서버(npm start, 기본 포트 5173). 먼저 ./build.sh 필요
#   ./start.sh --dev          개발 서버(npm run dev)
#   ./start.sh --port 8080    포트 지정 (기본 5173)
#   ./start.sh --host 0.0.0.0 다른 PC 에서도 접속 (기본은 127.0.0.1 전용)
#   ./start.sh --fg           백그라운드로 보내지 않고 이 터미널에서 실행 (Ctrl+C 로 종료)
# 종료: ./stop.sh     로그: tail -f .run/cg-editor.log
set -e
cd "$(dirname "$(readlink -f "$0")")"

MODE=prod; PORT=""; HOST=""; FG=0
while [ $# -gt 0 ]; do
  case "$1" in
    --prod) MODE=prod ;;
    --dev) MODE=dev ;;
    --port) PORT="$2"; shift ;;
    --host) HOST="$2"; shift ;;
    --fg) FG=1 ;;
    -h|--help) sed -n '2,9p' "$0"; exit 0 ;;
    *) echo "알 수 없는 옵션: $1"; exit 1 ;;
  esac
  shift
done
[ -z "$PORT" ] || [[ "$PORT" =~ ^[0-9]+$ ]] || { echo "--port 는 숫자여야 합니다"; exit 1; }

RUN=.run; PIDF=$RUN/cg-editor.pid; LOG=$RUN/cg-editor.log; FPIDF=$RUN/cg-files.pid; FLOG=$RUN/cg-files.log
mkdir -p "$RUN"

if [ -f "$PIDF" ] && kill -0 "$(cat "$PIDF")" 2>/dev/null; then
  echo "이미 실행 중입니다 (pid $(cat "$PIDF")). 먼저 ./stop.sh"
  exit 1
fi
rm -f "$PIDF"

[ -d node_modules ] || { echo "node_modules 가 없습니다. 먼저 ./build.sh 를 실행하세요."; exit 1; }

# 환경변수: .env(공통) -> prod 모드면 .env.production 이 덮어씀 -> --port/--host 옵션이 최우선
set -a
[ -f .env ] && . ./.env
[ "$MODE" = prod ] && [ -f .env.production ] && . ./.env.production
set +a
[ -n "$PORT" ] || PORT="${CG_EDITOR_PORT:-}"
[ -n "$HOST" ] || HOST="${CG_EDITOR_HOST:-}"
# 실제로 쓴 포트를 기록: 설정(cgsetup.cfg editor_port 등)을 바꾼 뒤에도 stop.sh 가 예전 포트의 서버를 정리할 수 있게 함
[ -z "$PORT" ] || echo "$PORT" > "$RUN/cg-editor.port"

ARGS=()
if [ "$MODE" = prod ]; then
  [ -d .next ] || { echo ".next/ 가 없습니다. 먼저 ./build.sh 를 실행하세요."; exit 1; }
  CMD=(npm start)
  [ -z "$PORT" ] || ARGS+=(--port "$PORT")
  [ -z "$HOST" ] || ARGS+=(--hostname "$HOST")
else
  CMD=(npm run dev)
  [ -z "$PORT" ] || ARGS+=(--port "$PORT")
  [ -z "$HOST" ] || ARGS+=(--hostname "$HOST")
fi
[ ${#ARGS[@]} -eq 0 ] || CMD+=(-- "${ARGS[@]}")

# 파일 서버(bin/media 목록·스트리밍, bin/project 저장/열기). 에디터가 디스크에 접근하려면 필요.
#   포트 8081(CG_FILES_PORT). 에디터를 --host 로 열면 파일 서버도 같은 주소로 연다.
[ -z "$HOST" ] || export CG_FILES_HOST="$HOST"
if [ -f "$FPIDF" ] && kill -0 "$(cat "$FPIDF")" 2>/dev/null; then
  echo "파일 서버는 이미 실행 중 (pid $(cat "$FPIDF"))"
else
  setsid nohup node scripts/files-server.mjs >"$FLOG" 2>&1 < /dev/null &
  echo $! > "$FPIDF"
fi

if [ "$FG" = 1 ]; then
  trap 'kill -TERM -- "-$(cat "$FPIDF" 2>/dev/null)" 2>/dev/null; rm -f "$FPIDF"' EXIT
  "${CMD[@]}"
  exit $?
fi

# setsid: 새 프로세스 그룹으로 띄워 stop.sh 가 하위 프로세스까지 한 번에 종료할 수 있게 한다
setsid nohup "${CMD[@]}" >"$LOG" 2>&1 < /dev/null &
echo $! > "$PIDF"
sleep 2
if kill -0 "$(cat "$PIDF")" 2>/dev/null; then
  echo "시작됨 ($MODE, pid $(cat "$PIDF")). 주소는 로그에 표시됩니다:"
  grep -m3 -E "http://|Local:" "$LOG" || echo "  (아직 준비 중) tail -f $LOG"
else
  echo "시작 실패. 로그:"; tail -n 20 "$LOG"; rm -f "$PIDF"; exit 1
fi
