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
