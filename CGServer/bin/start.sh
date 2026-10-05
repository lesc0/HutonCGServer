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
#   CG_PROJECT=자막프로젝트     송출할 프로젝트 이름 (bin/project/<이름>.json). 지정하지 않으면 마지막으로 적용한(Switch project) 프로젝트(.run/last-project),
#                               기록이 없는 첫 실행이면 빈 프로젝트(엔진 대기 상태)로 시작
#   CG_UDP=10.10.10.18:1234    cgsetup.cfg 의 udp_ip/udp_port 대신 쓸 목적지 (임시 테스트용, 지정 시 cfg보다 우선)
#   CG_EDITOR_PORT=8080        cg-editor 포트 (기본: cgsetup.cfg 의 editor_port)
#   CG_SKIP_STREAMER=1 / CG_SKIP_KIOSK=1   cg-streamer / 키오스크는 시작하지 않음 (rebuild.sh 가 사용)
#   DISPLAY=:0                 Chromium·해상도 설정에 쓸 X 디스플레이
set -u
cd "$(dirname "$(readlink -f "$0")")"
mkdir -p log .run

PROJECT=${CG_PROJECT:-$(cat .run/last-project 2>/dev/null | head -1 | tr -d '\r\n')}
[ -n "$PROJECT" ] && [ -f "project/$PROJECT.json" ] || PROJECT=   # 기록이 없거나 파일이 사라졌으면 빈 프로젝트로 시작(없는 경로를 주면 엔진은 대기 상태)
# 에디터 포트: 환경변수 CG_EDITOR_PORT > cgsetup.cfg 의 editor_port > cg-editor/.env.production > 5173
CFG_PORT=$(sed -n 's/#.*//; s/^[[:space:]]*editor_port[[:space:]]*=[[:space:]]*\([0-9][0-9]*\).*/\1/p' cgsetup.cfg 2>/dev/null | tail -1)
PORT=${CG_EDITOR_PORT:-${CFG_PORT:-$(set -a; . ../src/cg-editor/.env 2>/dev/null; . ../src/cg-editor/.env.production 2>/dev/null; echo "${CG_EDITOR_PORT:-5173}")}}
export DISPLAY=${DISPLAY:-:0}
EDITOR_DIR=../src/cg-editor
CHROMIUM=/opt/chromium.org/stable/chromium-browser
KIOSK_PROFILE=/tmp/cg-editor-kiosk-$(id -un)   # 사용자별 폴더(root 로 만든 폴더가 남아 있으면 다른 계정이 못 써서 Chromium 이 안 뜸)
CHROMIUM_PIDF=.run/kiosk-chromium.pid

echo "[1/3] cg-editor"
if curl -s -o /dev/null --max-time 2 "http://localhost:$PORT/"; then
  echo "  이미 실행 중"
else
  (cd "$EDITOR_DIR" && ./start.sh --port "$PORT") || echo "  시작 실패 (수동으로 $EDITOR_DIR/start.sh 확인)"
fi

# 로컬 화면(HDMI) 해상도 자동 설정 (display.sh). cg-streamer 의 로컬 미리보기(output=3) 창이 X 화면 크기로 만들어지므로 스트리머를 띄우기 *전에* 한다.

# X 서버가 없으면(lightdm 을 내리고 CG_DRM_OUT 빌드로 HDMI-2 에 DRM 직접 출력하는 구성) display.sh(xrandr)는 건너뛰고,
# CEF 는 X 없이 headless ozone + 소프트웨어 렌더로 띄운다(headless 에서 --gpu 는 paint 가 60/s 를 넘고 drop 이 계속 늘어 쓰지 않음).
# CG_NO_X=1 / 0 으로 자동 판단을 바꿀 수 있다.
if [ -z "${CG_NO_X:-}" ]; then pgrep -x Xorg >/dev/null || pgrep -x atomic-xorg >/dev/null && CG_NO_X=0 || CG_NO_X=1; fi
if [ "$CG_NO_X" = 1 ]; then
  echo "  X 서버 없음: DRM 직접 출력 모드(headless CEF, display.sh 생략)"
  CEF_GFX_ARGS=(--cef:ozone-platform=headless)
else
  bash ./display.sh
  CEF_GFX_ARGS=(--gpu --cef:use-angle=gles-egl)
fi

if [ "${CG_SKIP_STREAMER:-0}" = 1 ]; then echo "[2/3] cg-streamer 건너뜀(CG_SKIP_STREAMER=1)"; else
# 표시/인코딩/음성 스레드를 SCHED_FIFO 로 올리려면 실시간 우선순위 한도가 필요하다(없으면 엔진이 일반 스케줄링으로 동작).
# 한 번만: sudo sh -c 'echo "pi - rtprio 90" > /etc/security/limits.d/99-cg-rt.conf' 후 다시 로그인.
[ "$(ulimit -r)" = 0 ] && echo "  참고: 실시간 우선순위(ulimit -r)가 0 이라 SCHED_FIFO 를 쓸 수 없음 (위 limits.d 설정 필요)"
echo "[2/3] cg-streamer (project=${PROJECT:-(빈 프로젝트)}, 송출 설정은 cgsetup.cfg${CG_UDP:+", udp=$CG_UDP(override)"})"
if pgrep -f "cg-streamer --run" >/dev/null; then
  echo "  이미 실행 중"
else
  nohup ./cg-streamer --run --project="project/${PROJECT:-.none}.json" ${CG_UDP:+--udp="$CG_UDP"} "${CEF_GFX_ARGS[@]}" --autoplay \
    > log/cg-streamer.out 2>&1 &   # 정상 로그는 cg-streamer 가 직접 log/cg-streamer-YYYY-MM-DD.log 에 남김. .out 은 시작 직후 실패 등 그 이전 출력용
  disown
  sleep 2
  pgrep -f "cg-streamer --run" >/dev/null && echo "  시작됨" || echo "  시작 실패 (log/cg-streamer.out, log/cg-streamer-날짜.log 확인)"
fi
fi

# 에디터 키오스크는 cgsetup.cfg 의 editor_kiosk=on 일 때만 띄운다(기본 off: 이 단말의 데스크탑에서 브라우저를 직접 띄워 씀).
KIOSK_CFG=$(sed -n 's/#.*//; s/^[[:space:]]*editor_kiosk[[:space:]]*=[[:space:]]*(.*[^[:space:]])[[:space:]]*$//p' cgsetup.cfg 2>/dev/null | tail -1)
if [ "${CG_SKIP_KIOSK:-0}" = 1 ]; then echo "[3/3] Chromium 키오스크 건너뜀(CG_SKIP_KIOSK=1)"
elif [ "${KIOSK_CFG:-off}" != on ]; then echo "[3/3] Chromium 키오스크 건너뜀(cgsetup.cfg editor_kiosk=off)"
else
echo "[3/3] Chromium 키오스크 (cg-editor 화면)"
bash ./kiosk.sh start "$PORT"   # 에디터 모니터(.run/display-editor.geom) 위치에 띄움. 모니터 배치가 바뀌면 display.sh 가 kiosk.sh restart 로 다시 띄움
fi
