# 작업 기록

날짜별로 정리하며, 최근 날짜가 맨 위에 옵니다.

## 2026-10-01

### cg-editor3(doc/gptcode) 수정분 병합
- 내용: Stamp Playback(두 자막 채널, Top/Bottom 배치, 독립 재생·정지·반복·갱신), Global Animation Playback(배경 영상, On Air, 재생·일시정지·되감기, 구간 재생), 채널 설정·미디어 JSON 저장/복원.
- 방식: 덮어쓰기 없이 3-way 병합. 기준은 `CG-editor-source2.zip`, 내 쪽은 기존 src/cg-editor, 상대는 cg-editor3. 충돌 없음.
- 대상: app/channels.tsx(신규), editor-canvas.tsx, globals.css, model.ts, page.tsx, tests/editor-checks.ts, 다운로드-실행안내.txt.
- 확인: `tsc --noEmit` 통과. 화면 동작은 브라우저 확인 필요. 커밋/푸시는 아직 안 함.

### cg-editor2(doc/gptcode) 수정분 병합
- 내용: Run Setting(페이지 구간·반복 횟수·대기·자동/수동), Playback(Clear·Cut·Skip·이전/다음), 효과 프리셋 수·Soft/Hard 보완.
- 방식: 덮어쓰기 없이 3-way 병합. 기준(base)은 `doc/gptcode/CG-editor-source.zip`, 내 쪽은 기존 src/cg-editor, 상대는 cg-editor2.
- 대상: app/attributes.tsx, editor-canvas.tsx, effects.ts, globals.css, model.ts, page.tsx, tests/editor-checks.ts, package-lock.json, 다운로드-실행안내.txt.
- 유지한 기존 변경: 패널 높이 조절(rowsplit), 메뉴 바깥 클릭 닫기, Effect 체크 해제.
- 확인: `tsc --noEmit` 통과. 화면 동작은 브라우저에서 확인 필요.
- 백업: 병합 전 app/, tests/를 세션 임시 폴더에 복사해 둠.

## 2026-09-30

### 메인 메뉴가 바깥을 눌러도 닫히지 않는 문제
- 증상: 상단 메뉴(파일/편집/보기 …)를 열고 다른 곳을 눌러도 드롭다운이 계속 열려 있음.
- 원인: [app/page.tsx](app/page.tsx)의 메뉴는 메뉴 버튼 토글(`setMenu(menu===name?null:name)`)과 항목 선택 시에만 닫혔고, 바깥 클릭 처리가 없었음.
- 변경: `menu`가 열려 있는 동안 동작하는 `useEffect` 추가 (`openModal` 바로 위).
  - `pointerdown` / `mousedown` / `touchstart`(capture)에서 대상이 `.menugroup` 밖이면 `setMenu(null)`
  - `Escape` 키, 창 `blur` 시에도 닫힘
  - 실행 취소/다시 실행 기록(history) 로직은 변경하지 않음
- 확인: 개발 서버(5173)가 수정된 코드를 서빙하는 것과 VS Code 진단 오류 없음까지 확인.
- 미해결: 사용자가 "메뉴를 두 번 눌러야 없어진다"고 보고. 브라우저에서 직접 재현하지 못해 원인 미확정.
  - 확인 필요: 강력 새로고침(Ctrl+Shift+R) 후에도 동일한지, 바깥 클릭(a)인지 메뉴 버튼 재클릭(b)인지, 어느 영역(캔버스/패널/툴바)에서 발생하는지.

### 참고
- `src/editor/client/src/TopBar.jsx`에도 별도의 상단 메뉴가 있으나(헤더에서 마우스가 벗어나면 닫힘) 이번에는 수정하지 않음.
