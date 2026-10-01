#!/bin/bash
# cg-editor 종료 (start.sh 가 만든 .run/cg-editor.pid 의 프로세스 그룹을 종료)
#   ./stop.sh
cd "$(dirname "$(readlink -f "$0")")"
PIDF=.run/cg-editor.pid
FPIDF=.run/cg-files.pid

# 파일 서버 종료 (에디터 pid 파일이 없어도 정리)
if [ -f "$FPIDF" ]; then
  FPID=$(cat "$FPIDF")
  kill -TERM -- "-$FPID" 2>/dev/null || kill -TERM "$FPID" 2>/dev/null
  rm -f "$FPIDF"
  echo "파일 서버 종료"
fi

if [ ! -f "$PIDF" ]; then
  echo "실행 중이 아닙니다 (pid 파일 없음)"
  exit 0
fi
PID=$(cat "$PIDF")
if ! kill -0 "$PID" 2>/dev/null; then
  echo "이미 종료되어 있습니다 (오래된 pid $PID 정리)"
  rm -f "$PIDF"
  exit 0
fi

# setsid 로 시작했으므로 PID == 프로세스 그룹 ID. 그룹 전체에 SIGTERM
kill -TERM -- "-$PID" 2>/dev/null || kill -TERM "$PID" 2>/dev/null
for _ in $(seq 1 20); do
  kill -0 "$PID" 2>/dev/null || break
  sleep 0.25
done
if kill -0 "$PID" 2>/dev/null; then
  echo "정상 종료되지 않아 강제 종료합니다"
  kill -KILL -- "-$PID" 2>/dev/null || kill -KILL "$PID" 2>/dev/null
fi
rm -f "$PIDF"
echo "종료했습니다"
