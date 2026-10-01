# 작업 기록

## 2026-10-01 — cg-streamer 가로스크롤 끊김 원인 발견: 진짜 Mali GPU 가속 미사용

### 증상
가로스크롤(크롤) 자막이 HDMI 로컬 출력/UDP(VLC) 양쪽 다 끊겨 보임.

### 조사 과정 (요약)
1. EBF(SendExternalBeginFrame), CSS transform/animation 전환, 캔버스 오프스크린 캐싱(drawImage)
   등 여러 방향을 시도했으나 체감 차이 없음 — 전부 "이미 공급 과잉(under=0)인 상태에서 공급을
   더 늘리는" 시도였던 것으로 뒤늦게 확인됨 (근본 원인이 아니었음).
2. OnPaint 원본을 mp4로 직접 떠서 비교하는 진단(cef_dumper)도 시도했으나, 처음엔 동기 인코딩이
   OnPaint를 블로킹해 측정 자체가 왜곡됨 — 비동기(워커 스레드)로 고쳐도 `--no-encode` 유휴 상태에서는
   시스템이 CPU 클럭을 낮춰서 여전히 신뢰할 수 없는 숫자가 나옴. 이 진단 방식 자체를 폐기.
3. 기본 실행 시 `disable-gpu`/`disable-gpu-compositing` 가 항상 붙어서(main.cpp) CEF가 완전
   소프트웨어 렌더링(paint≈11fps)으로 떨어지는 걸 발견 → `--gpu` 플래그로 이걸 끄니 paint가
   75~120fps 로 올라감. 그런데 이것도 사실은 **ANGLE이 SwiftShader(소프트웨어)로 폴백**한
   상태에서의 개선이었음 (`--use-gl=angle --use-angle=swiftshader-webgl` 로 떠 있는 걸 확인).
4. `/root/work/github.cgserver/CGServer/cef_server_build.md` 문서를 재확인 — 이 보드가 쓰는
   커스텀 CEF 130은 **Mali DDK 패치**(`0016 HACK-ui-x11-Fix-config-choosing-error-with-Mali-DDK`)가
   포함된 빌드였고, 문서에 정확한 사용법이 이미 적혀 있었음: `--cef:use-angle=gles-egl`.
   이걸 안 쓰고 있었던 게 진짜 원인.

### 근본 원인
`--gpu` 만으로는 ANGLE이 기본 GL 백엔드(Mesa/GLX)를 시도하다 이 보드의 proprietary Mali 블롭용
Mesa DRI 드라이버가 없어서 실패(`failed to load driver: rockchip`)하고, 자동으로 SwiftShader(CPU
소프트웨어 렌더링)로 폴백하고 있었음. `--cef:use-angle=gles-egl` 을 명시해야 커스텀 CEF 빌드가
제공하는 진짜 Mali 패스스루 경로를 탐.

### 적용한 수정
- `bin/start.sh`: cg-streamer 실행 인자에 `--gpu --cef:use-angle=gles-egl` 기본 추가.
- 커밋: `e9ea8fe` (cg-streamer: 진짜 Mali GPU 가속 활성화)

### 결과
- SwiftShader 상태: paint 75~120fps(과공급, drop 지속 증가), CPU 부하 큼.
- gles-egl 적용 후: **paint 가 설정 fps(60)에 정확히 맞춰짐, drop=0**, GPU 프로세스가 크래시/
  재시작 없이 한 번에 뜸, **CPU 사용량 체감 절반으로 감소**(사용자 확인).
- 가로스크롤 끊김 체감 해소.

### 참고
- 시도했으나 효과 없었던 것들(참고용, 다시 안 해봐도 됨): EBF, CSS 애니메이션 전환, 캔버스
  drawImage 캐싱. 전부 원인이 아니었고, 캔버스 drawImage 캐싱만 부작용 없이 남겨둠(성능상 이득은
  있으나 끊김의 직접 원인은 아니었음).
- `--use-gl=egl`(ANGLE 안 거치는 순정 EGL)은 이 CEF 빌드에 아예 컴파일 안 되어 있어서 불가.
- 이 보드 Mali 드라이버(`libmali-valhall-g610-g13p0-x11-gbm`)는 Vulkan 미지원.
