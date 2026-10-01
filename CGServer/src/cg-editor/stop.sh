#!/bin/bash
# cg-editor 종료: 에디터 + 파일 서버. pid 파일의 프로세스 그룹을 종료하고,
# setsid 로 pid 가 어긋나거나 리더만 먼저 죽어 자식이 남는 경우를 대비해 포트(에디터/파일 서버)를 잡고 있는 프로세스도 정리한다.
#   ./stop.sh
cd "$(dirname "$(readlink -f "$0")")"
PIDF=.run/cg-editor.pid
FPIDF=.run/cg-files.pid

# start.sh 와 같은 순서로 .env 를 읽어 포트를 구한다 (.env -> .env.production)
GIVEN_PORT=${CG_EDITOR_PORT:-}
set -a
[ -f .env ] && . ./.env
[ -f .env.production ] && . ./.env.production
set +a
EPORT=${GIVEN_PORT:-${CG_EDITOR_PORT:-5173}}
FILES_PORT=${CG_FILES_PORT:-8081}
LAST_PORT=$(cat .run/cg-editor.port 2>/dev/null)   # start.sh 가 마지막으로 쓴 포트(설정이 바뀌었어도 예전 서버를 정리)

listening() { # 포트 $@ 중 하나라도 LISTEN 중이면 0 (소유자와 무관하게 확인: fuser 는 다른 사용자 프로세스를 못 봄)
  local p; for p in "$@"; do [ -n "$(ss -ltnH "sport = :$p" 2>/dev/null)" ] && return 0; done; return 1
}

killgroup() { # $1=pid : 그룹 전체에 TERM (리더가 이미 죽었어도 그룹이 남아 있으면 종료됨)
  kill -TERM -- "-$1" 2>/dev/null || kill -TERM "$1" 2>/dev/null
}

for f in "$FPIDF" "$PIDF"; do
  [ -f "$f" ] && killgroup "$(cat "$f")"
done

# 포트 기준 정리 (pid 파일이 틀려도 확실히 종료)
for p in "$EPORT" "$LAST_PORT" "$FILES_PORT"; do
  [ -n "$p" ] || continue
  fuser -k -TERM "$p"/tcp >/dev/null 2>&1
done
for _ in $(seq 1 20); do
  listening "$EPORT" ${LAST_PORT:+"$LAST_PORT"} "$FILES_PORT" || break
  sleep 0.25
done
for p in "$EPORT" "$FILES_PORT"; do
  fuser -k -KILL "$p"/tcp >/dev/null 2>&1
done
rm -f "$PIDF" "$FPIDF" .run/cg-editor.port

if listening "$EPORT" ${LAST_PORT:+"$LAST_PORT"} "$FILES_PORT"; then
  echo "종료하지 못한 프로세스가 있습니다 (다른 사용자(root) 소유일 수 있음): sudo ./stop.sh"
  exit 1
fi
echo "종료했습니다 (에디터 :$EPORT, 파일 서버 :$FILES_PORT)"
