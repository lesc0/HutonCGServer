#!/bin/bash
# cg-editor 빌드: 의존성 설치(필요할 때) + 프로덕션 빌드(.next/)
#   ./build.sh            node_modules 가 없으면 npm ci 후 빌드
#   ./build.sh --install  node_modules 를 항상 npm ci 로 새로 설치한 뒤 빌드
#   ./build.sh --check    빌드 전에 타입 검사(tsc --noEmit)도 실행
set -e
cd "$(dirname "$(readlink -f "$0")")"

INSTALL=0; CHECK=0
for a in "$@"; do
  case "$a" in
    --install) INSTALL=1 ;;
    --check) CHECK=1 ;;
    -h|--help) sed -n '2,5p' "$0"; exit 0 ;;
    *) echo "알 수 없는 옵션: $a"; exit 1 ;;
  esac
done

command -v node >/dev/null || { echo "node 가 없습니다 (22.13 이상 필요)"; exit 1; }
node -e 'const [a,b]=process.versions.node.split(".").map(Number);process.exit(a>22||(a===22&&b>=13)?0:1)' \
  || { echo "Node.js 22.13 이상이 필요합니다 (현재 $(node -v))"; exit 1; }

if [ "$INSTALL" = 1 ] || [ ! -d node_modules ]; then
  echo "[build] npm ci"
  npm ci
fi
if [ "$CHECK" = 1 ]; then
  echo "[build] tsc --noEmit"
  npx tsc --noEmit
fi
echo "[build] npm run build"
npm run build
echo "[build] 완료: .next/  (실행: ./start.sh --prod)"
