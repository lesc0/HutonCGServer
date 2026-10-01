폰트 폴더
=========
여기에 폰트 파일(.ttf .otf .woff .woff2)을 넣으면 에디터의 글꼴 목록과 송출 엔진(cg-streamer)에 자동으로 나타납니다.

- 파일 이름이 글꼴 이름이 됩니다.            예) NanumGothic.ttf  ->  NanumGothic
- 굵게/기울임 파일은 이름 끝에 -Bold, -Italic, -BoldItalic 을 붙이면 같은 글꼴의 Bold/Italic 으로 묶입니다.
                                                예) NanumGothic-Bold.ttf  ->  NanumGothic (Bold)
- 한글 이름도 됩니다.                         예) 나눔고딕.ttf
- 라이선스(상업적 이용 가능 여부)는 직접 확인하세요. 예: SIL OFL 로 배포되는 구글 폰트는 방송에 써도 됩니다.

반영 방법
- 에디터: 글꼴 목록은 에디터 화면을 새로고침(F5)하면 다시 읽습니다.
- 송출 엔진: 폰트를 추가한 뒤 cg-streamer 를 다시 시작해야 합니다(bin/stop.sh, bin/start.sh).
- fonts.css 는 파일 서버가 이 폴더를 읽어 자동으로 만들므로 직접 고치지 마세요(git 에도 올라가지 않음).

기본 포함 폰트 (모두 SIL Open Font License 1.1 — 상업 방송 사용 가능, 폰트 단독 판매만 금지)
- 고딕/본문 : NanumGothic, NotoSansKR, GothicA1, IBMPlexSansKR, GowunDodum
- 명조/바탕 : NanumMyeongjo, GowunBatang
- 제목/장식 : BlackHanSans, DoHyeon, Jua, Gugi, YeonSung
- 손글씨    : NanumPenScript, Gaegu, PoorStory, HiMelody
- 구글 폰트 저장소(github.com/google/fonts, ofl/)에서 받아 .woff2 로 변환했습니다. NotoSansKR 은 가변 폰트를 400/700 으로 나눈 것입니다.
- BlackHanSans, DoHyeon, Jua, Gugi, Gaegu, YeonSung 은 상용 한글 2,350자만 들어 있어 드문 글자는 대체 글꼴로 나옵니다.
- 각 폰트의 라이선스 전문은 licenses/ 폴더에 있습니다. 이 폴더를 다른 곳에 옮기거나 배포할 때 함께 두세요.
