#!/bin/bash
# 보드 화면에 지금과 같은 구성으로 띄우기: cg-editor(웹) + cg-streamer(UDP 송출, 로컬 미리보기 없음) + Chromium 키오스크(cg-editor 화면).
# cg-streamer는 --gpu --cef:use-angle=gles-egl 로 띄움:
#   --gpu 없으면 CEF가 GPU를 꺼서(disable-gpu-compositing) 페인트가 ~11fps로 떨어짐(가로스크롤 등 끊김의 원인).
#   --cef:use-angle=gles-egl 없으면 ANGLE이 기본 GL(Mesa/GLX) 경로를 시도하다 실패해서 결국 SwiftShader(소프트웨어)로
#   폴백함 - 이 보드용 커스텀 CEF(cef_server_build.md 참고, Mali DDK 패치 포함)는 gles-egl 로 줘야 진짜 Mali GPU를 씀.
#   gles-egl 적용 시 paint가 정확히 설정 fps(60)로 나오고 drop=0 (SwiftShader는 75~120fps로 과공급+드롭 발생).
#   ./start.sh     모두 시작 (이미 떠 있으면 건너뜀)
# 종료: ./stop.sh
#
# 송출 설정(output 1/2/3, udp_ip, udp_port)과 에디터 포트(editor_port)는 cgsetup.cfg 에서 관리.
# 환경변수로 바꿀 수 있음 (기본값은 지금 쓰던 값):
#   CG_PROJECT=자막프로젝트     송출할 프로젝트 이름 (bin/project/<이름>.json)
#   CG_UDP=10.10.10.18:1234    cgsetup.cfg 의 udp_ip/udp_port 대신 쓸 목적지 (임시 테스트용, 지정 시 cfg보다 우선)
#   CG_EDITOR_PORT=8080        cg-editor 포트 (기본: cgsetup.cfg 의 editor_port)
#   CG_SKIP_STREAMER=1 / CG_SKIP_KIOSK=1   cg-streamer / 키오스크는 시작하지 않음 (rebuild.sh 가 사용)
#   DISPLAY=:0                 Chromium·해상도 설정에 쓸 X 디스플레이
set -u
cd "$(dirname "$(readlink -f "$0")")"
mkdir -p log .run

PROJECT=${CG_PROJECT:-자막프로젝트}
# 에디터 포트: 환경변수 CG_EDITOR_PORT > cgsetup.cfg 의 editor_port > cg-editor/.env.production > 5173
CFG_PORT=$(sed -n 's/#.*//; s/^[[:space:]]*editor_port[[:space:]]*=[[:space:]]*\([0-9][0-9]*\).*/\1/p' cgsetup.cfg 2>/dev/null | tail -1)
PORT=${CG_EDITOR_PORT:-${CFG_PORT:-$(set -a; . ../src/cg-editor/.env 2>/dev/null; . ../src/cg-editor/.env.production 2>/dev/null; echo "${CG_EDITOR_PORT:-5173}")}}
export DISPLAY=${DISPLAY:-:0}
EDITOR_DIR=../src/cg-editor
CHROMIUM=/opt/chromium.org/stable/chromium-browser
KIOSK_PROFILE=/tmp/cg-editor-kiosk
CHROMIUM_PIDF=.run/kiosk-chromium.pid

echo "[1/3] cg-editor"
if curl -s -o /dev/null --max-time 2 "http://localhost:$PORT/"; then
  echo "  이미 실행 중"
else
  (cd "$EDITOR_DIR" && ./start.sh --port "$PORT") || echo "  시작 실패 (수동으로 $EDITOR_DIR/start.sh 확인)"
fi

# 로컬 화면(HDMI) 해상도 자동 설정 (display.sh). cg-streamer 의 로컬 미리보기(output=3) 창이 X 화면 크기로 만들어지므로 스트리머를 띄우기 *전에* 한다.
bash ./display.sh

if [ "${CG_SKIP_STREAMER:-0}" = 1 ]; then echo "[2/3] cg-streamer 건너뜀(CG_SKIP_STREAMER=1)"; else
echo "[2/3] cg-streamer (project=$PROJECT, 송출 설정은 cgsetup.cfg${CG_UDP:+", udp=$CG_UDP(override)"})"
if pgrep -f "cg-streamer --run" >/dev/null; then
  echo "  이미 실행 중"
else
  nohup ./cg-streamer --run --project="project/$PROJECT.json" ${CG_UDP:+--udp="$CG_UDP"} --gpu --cef:use-angle=gles-egl --autoplay \
    > log/cg-streamer.log 2>&1 &
  disown
  sleep 2
  pgrep -f "cg-streamer --run" >/dev/null && echo "  시작됨" || echo "  시작 실패 (log/cg-streamer.log 확인)"
fi
fi

if [ "${CG_SKIP_KIOSK:-0}" = 1 ]; then echo "[3/3] Chromium 키오스크 건너뜀(CG_SKIP_KIOSK=1)"; else
echo "[3/3] Chromium 키오스크 (cg-editor 화면)"
if [ -f "$CHROMIUM_PIDF" ] && kill -0 "$(cat "$CHROMIUM_PIDF")" 2>/dev/null; then
  echo "  이미 실행 중"
else
  DISPLAY="$DISPLAY" nohup "$CHROMIUM" --kiosk --noerrdialogs --disable-infobars --no-first-run \
    --user-data-dir="$KIOSK_PROFILE" "http://localhost:$PORT" \
    > log/chromium-kiosk.log 2>&1 &
  echo $! > "$CHROMIUM_PIDF"
  disown
  echo "  시작됨 (pid $(cat "$CHROMIUM_PIDF"))"
fi
fi
