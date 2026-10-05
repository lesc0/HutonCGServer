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
- 고딕/본문 : NanumGothic, NotoSansKR(Light/Regular/Medium/Bold/Black), Pretendard(Light/Regular/Medium/Bold/Black), SpoqaHanSansNeo(Light/Regular/Medium/Bold), GothicA1, IBMPlexSansKR, GowunDodum
- 명조/바탕 : NanumMyeongjo, GowunBatang
- 제목/장식 : BlackHanSans, DoHyeon, Jua, Gugi, YeonSung
- 손글씨    : NanumPenScript, Gaegu, PoorStory, HiMelody
- 구글 폰트 저장소(github.com/google/fonts, ofl/)에서 받아 .woff2 로 변환했습니다. NotoSansKR 은 가변 폰트를 굵기별(300/400/500/700/900)로 나눈 것입니다.
- Pretendard 는 github.com/orioncactus/pretendard 릴리스(v1.3.9)의 woff2, SpoqaHanSansNeo 는 github.com/spoqa/spoqa-han-sans 릴리스(v3.3.0)의 TTF 를 woff2 로 변환한 것입니다(둘 다 SIL OFL 1.1). Pretendard 는 한글 전체 글리프, SpoqaHanSansNeo 는 용량 때문에 한글 11,172자·영문·기호만 남기고 한자는 뺀 서브셋입니다(한자는 대체 글꼴로 나옵니다).
- Light/Medium/Black 처럼 Regular/Bold 가 아닌 굵기는 '이름-Light' 형태의 별개 글꼴(예: NotoSansKR-Light)로 등록됩니다. 에디터 글꼴 목록에는 보기 좋은 이름으로 보입니다(한글 폰트는 한글 이름 나눔고딕, 영문 이름 폰트는 영어 Noto Sans KR Light)(저장되는 값은 파일 이름 기반 글꼴 이름 그대로).
- BlackHanSans, DoHyeon, Jua, Gugi, Gaegu, YeonSung 은 상용 한글 2,350자만 들어 있어 드문 글자는 대체 글꼴로 나옵니다.
- Arial, 맑은 고딕, 돋움, 바탕, 궁서, Georgia, Times New Roman 같은 윈도우 전용 폰트는 단말(Debian)에 없고 배포할 수도 없어서 글꼴 목록에서 뺐습니다(대신 위 무료 폰트를 쓰세요: 돋움→나눔고딕, 바탕→나눔명조 등).
- 각 폰트의 라이선스 전문은 licenses/ 폴더에 있습니다. 이 폴더를 다른 곳에 옮기거나 배포할 때 함께 두세요.
