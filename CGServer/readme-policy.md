# 정책서

`cg-streamer`(송출 엔진, `src/cg-streamer`)와 송출 화면 런타임(`bin/web/cg-runtime.js`)이 **현재 코드에서 실제로 하는 동작**을 정리한 문서이다(2026-10-05 기준). 구현되지 않은 계획은 적지 않고, 알려진 한계는 맨 끝에 모았다. 값의 출처는 `main.cpp`(EncodeLoop·지터 버퍼), `hdmirx_source.cpp`, `video_source.cpp`, `audio_mixer.cpp`, `ts_muxer.cpp`, `mpp_encoder.cpp`, `log_writer.cpp`, `cg-runtime.js` 이다.

## 1. 시계

| 항목 | 정책 |
|---|---|
| 기준 시계 | 모든 시각은 `CLOCK_MONOTONIC`(부팅 후 경과 시간) 하나로 맞춘다. `std::steady_clock`, `timerfd`, V4L2 버퍼 timestamp(`MONOTONIC`), ALSA tstamp(`MONOTONIC`)가 모두 같은 시계이다. |
| 기준점 | 공통 기준 시각(`t0`)을 빼지 않는다. PTS 값은 시스템 가동 시간을 그대로 90kHz 로 환산한 값이다. |

## 2. 송출 틱

| 항목 | 정책 |
|---|---|
| 틱 생성 | `EncodeLoop` 가 `timerfd_create(CLOCK_MONOTONIC)` 로 틱을 만든다. 주기는 `1e9 × den / num` ns 이고 절대 격자로 만료된다. |
| fps 설정 | `cgsetup.cfg` 의 `fps=` 는 29.97 / 30 / 59.94 / 60 을 쓴다(그 밖의 1~240 소수도 읽음, 해석 못 하면 경고 후 기본값). 정수는 N/1, NTSC 계열은 N×1000/1001(29.97 = 30000/1001, 59.94 = 60000/1001)로 보관한다. 기본 60. |
| 분수 fps 의 적용 범위 | 인코더 rate control, TS muxer(`avg_frame_rate`, 패킷 duration), `EncodeLoop` 틱 주기는 분수 그대로. **CEF 페인트 fps·통계 주기·GOP 는 반올림한 정수**(29.97 → 30, 59.94 → 60)를 쓴다(CEF 의 `windowless_frame_rate` 가 정수만 받기 때문). |
| 늦은 틱 | `timerfd` 만료 횟수가 1보다 크면 **한 번만 처리**하고 밀린 횟수를 `miss` 로 센다. 밀린 틱을 몰아서 따라잡지 않는다. |
| 실시간 우선순위 | `cgsetup.cfg` 의 `realtime=on` 일 때만 SCHED_FIFO 로 올린다(기본 off). `cg-encode` 50, `cg-preview` 49, `cg-hdmirx`·`cg-hdmi-aud`·`cg-audio` 48, `cg-audio-out` 47. 권한(`rtprio`)이 없으면 한 번 안내하고 일반 스케줄링으로 동작한다. 소프트웨어 틱의 정밀도는 실시간 우선순위에서 표준편차 수 µs, 일반 스케줄링의 부하 상태에서는 약 160µs(최대 ±3ms)이다. |
| 하드웨어 틱 | 쓰지 않는다. HDMI 입력 프레임 도착(V4L2), DRM vblank(출력 모니터가 있을 때), 오디오 클럭이 하드웨어 후보이나 틱으로는 `timerfd` 를 쓴다. CEF 는 vsync 가 없는 OSR 이라 자체 타이머로 돈다. |
| 시작 | 플레이어 로딩이 끝나기 전(`ready` 전)에는 인코딩하지 않는다. |

## 3. 영상 소스와 프레임 선택

매 틱에 **프레임 1장**을 인코딩한다.

| 항목 | 정책 |
|---|---|
| 소스 선택 | 페이지의 영상 개체가 `video:play:<대상>` 으로 요청한다. `hdmirx`(또는 `/dev/video*`)이면 HDMI 입력, 그 외 경로(프로젝트 기준 상대경로 가능)이면 영상 파일. 둘은 동시에 쓰지 않고 한쪽을 켜면 다른 쪽을 끈다. `video:stop` 으로 끈다. 영상 위치·크기는 `video:rect`. |
| CG 만 있을 때 | CEF 그림(1920×1080 BGRA)을 지터 버퍼(6절)에서 틱마다 1장 꺼내 RGA 로 NV12 변환. **새 그림이 없으면 직전 NV12 버퍼를 그대로 다시 인코딩**한다. |
| 영상 합성 | 영상(HDMI/파일) 위에 CG 를 얹는다. CG 는 새 그림일 때만 dmabuf 로 올리고, 매 틱 영상+CG 를 RGA 로 합성한다. |
| HDMI 입력 | V4L2 멀티플레인, 버퍼 4개(mmap, `VIDIOC_EXPBUF` dmabuf). 프레임이 올 때마다 `VIDIOC_DQBUF` 하고 **최신 프레임만 보관**하며 이전 버퍼는 곧바로 드라이버에 돌려준다. 틱이 그 순간의 최신 1장을 가져간다. `V4L2_BUF_FLAG_ERROR` 버퍼는 버린다. 포맷은 BGR24/NV12/NV16/NV24. 신호가 없으면 영상 자리는 검정(CG 는 유지), 해상도·신호가 바뀌거나 2초간 프레임이 없으면 다시 연결한다. |
| 영상 파일 | ffmpeg 로 분리 → MPP 하드웨어 디코딩(NV12). 원본 fps(`avg_frame_rate`, 없으면 `r_frame_rate`, 없으면 30)에 맞춰 **최신 프레임을 갱신**하고 끝나면 처음부터 반복한다. |
| 입력 fps ≠ 출력 fps | **입력 > 출력**(예: HDMI 입력 59.94p, 출력 30p): 틱마다 최신 1장만 쓰고 나머지는 버린다(솎아내기, 합성·인코딩은 출력 fps 만큼만 하므로 부하도 준다). **입력 < 출력**: 새 프레임이 없는 틱은 같은 프레임을 다시 쓴다(zero-order hold, 반복 프레임은 인코더가 거의 0비트로 압축). 움직임 보간·블렌딩은 하지 않는다. 입력과 출력이 거의 같으면서 정확히 같지는 않을 때(59.94 vs 60)는 주기적으로 한 프레임이 반복되거나 건너뛴다. |

## 4. 영상 PTS

| 항목 | 정책 |
|---|---|
| 변환 | muxer 는 `PTS = DTS = timestamp(ns) → 90kHz` 로 환산만 한다. 기준점 빼기·단조 보정·프레임 번호를 쓰지 않는다. timestamp 가 0 이면 그 시점의 지금 시각을 쓴다. |
| 라이브(HDMI) 새 프레임 | V4L2 `buf.timestamp`(µs). `MONOTONIC` 이 아니면 도착 시각으로 대체한다. 직전 값 이하면 직전 + 1 로 올린다(`hdmirx_source.cpp` 입력 쪽에서만, 재연결해도 유지). |
| 그 밖 | CG 만, 영상 파일, 같은 라이브 프레임을 다시 쓰는 틱(`ts_ns == last_live_ts`)은 **그 틱의 시각**(`timerfd` 가 깬 시각)을 쓴다. |
| 간격 | 입력 timestamp 를 그대로 쓰므로 PTS 간격은 일정하지 않을 수 있다(의도). |
| 영상 파일의 PTS | 파일의 프레임 PTS 는 쓰지 않는다. 표시 시각은 프레임 순번 / 원본 fps 이다. 음성 배치에는 컨테이너의 음성·영상 `start_time` 차이만 반영한다. |

## 5. 음성

| 항목 | 정책 |
|---|---|
| 형식 | AAC-LC 48kHz 스테레오 128kbps, 1024샘플 프레임. PMT 에 음성 스트림이 항상 있으므로 입력이 없으면 무음을 보낸다. |
| 라이브(HDMI 입력 음성) | ALSA `plughw:CARD=rockchiphdmiin,DEV=0`(S16 스테레오 48kHz)에서 1024샘플씩 읽는다. tstamp 모드를 켜고 타입을 `MONOTONIC` 으로 설정(start 전)한 뒤 `snd_pcm_status_get_tstamp` 의 µs 값을 **샘플 수 보정 없이 그대로 PTS 로** 쓴다(ns 로 환산해 muxer 가 1/48000 → 90kHz). |
| 라이브 송출 경로 | **믹서를 거치지 않고** 1024샘플 블록으로 모아 muxer 에 직접 보낸다(`WriteLiveDirect`). 마지막 호출 후 300ms 까지는 믹서의 송출을 건너뛴다. 믹서에는 계속 넣어 로컬 재생에 쓴다. `kLiveLatency`(0.12초)는 믹서 경로에서만 쓴다. |
| 믹서 | 영상 파일 음성·효과음을 약 5.5초 링에 재생 예정 시각(`PushAt`)으로 배치해 합산(±1 클리핑)하고, 1024샘플 블록 시작 시각(격자)을 PTS 로 보낸다. 이미 지난 구간은 버리고 3초보다 먼 미래는 받지 않는다. |
| 로컬 재생 | `output=3` 이면 같은 믹스를 ALSA 장치(`audio_out=auto` 는 연결된 HDMI 소리 카드)로 내보낸다. 큐가 8블록을 넘으면 버린다(송출 우선). |

## 6. CEF 렌더링과 지터 버퍼

| 항목 | 정책 |
|---|---|
| 페인트 fps | `windowless_frame_rate` = 정수. 기본은 `fps` 를 반올림한 값이고 `--paint-fps=N` 으로 낮출 수 있다(상한은 `fps`). CEF 문서상 **최대값**이라 못 그리면 더 낮게 나온다. 정지 화면에서도 `OnPaint` 는 계속 온다. |
| 그리기 | 소프트웨어 `OnPaint`(기본): 전체 프레임(8MB)을 버퍼 풀에 복사. GPU 합성은 `--gpu --cef:use-angle=gles-egl`(`start.sh` 기본). |
| 지터 버퍼 | `paint_fps == fps` 이고 `--no-sync` 가 아닐 때 쓴다. 그림이 5장 쌓이거나(`kUiPrime`) 첫 그림이 5틱 넘게 기다리면 출력을 시작한다. 틱마다 1장 꺼낸다(지연 약 5프레임). |
| 수위 관리 | 큐가 8장을 넘으면(`kUiMax`) 가장 오래된 그림을 즉시 버리고(`drop`), 수위가 5를 넘은 상태가 1초 넘게 이어지면 1장 버린다. 큐가 비면 직전 그림을 한 번 더 쓰고 애니메이션 중일 때만 `under` 로 센다. |
| 지터 버퍼를 안 쓸 때 | `paint_fps < fps`(예: `--paint-fps=30` 과 `fps=60`)이면 지터 버퍼 없이 최신 그림만 쓴다. 이 경우 CEF 타이머와 틱의 위상이 맞지 않아 새 그림이 나오는 간격이 불규칙해진다. 인코더도 같은 fps 로 맞추면(예: `fps=30`) 지터 버퍼가 동작해 안정적이다. |
| 외부 BeginFrame | `-DENABLE_EXTERNAL_BEGIN_FRAME=ON`(`CG_EBF`) 빌드에서만 가능한 실험 기능이고 기본은 꺼져 있다(시험 결과 더 불규칙해서 쓰지 않는다). |

## 7. 송출 화면 런타임(`bin/web/cg-runtime.js`)

| 항목 | 정책 |
|---|---|
| 그리기 | 매 프레임 캔버스를 지우고 현재 페이지의 모든 개체를 다시 그린다. 시간은 `requestAnimationFrame` 마다 `framePeriod` 균일 간격으로 진행하고 실제 시계에 천천히 맞춘다(`SMOOTH_DT`). |
| 텍스트 캐시 | 크롤·롤 텍스트는 물론 **효과가 `text`(글자가 점점 나타남)가 아니고 그림자가 없는 일반 텍스트·시계도** 오프스크린 캔버스에 한 번만 그려 두고 매 프레임 `drawImage` 한다(글자 단위 `fillText` 를 매 프레임 하지 않음). 그림자가 있는 일반 텍스트는 경계에서 잘리므로 직접 그린다. 캐시 키는 개체 id 이고 글자·서식(`runs` 포함)이 바뀌면 다시 만든다. |
| 캐시 미리 생성 | 프로젝트를 읽은 직후(재생 전) 모든 페이지의 캐시 대상 텍스트를 미리 만든다(첫 로드·다시 읽기 둘 다). 페이지가 바뀌는 순간의 100ms 넘는 튐을 막기 위해서이다. |
| 폰트 | 사용하는 글꼴은 그리기 전에 미리 읽는다(`document.fonts.load`, 굵게·기울임 조합). 폰트가 늦게 로드되면 캐시를 버린다(`loadingdone` 마다 `fontEpoch` 증가). |
| 크롤·롤 | 크롤 이동 거리는 (화면 폭 + 박스 폭) × `speed`, 시간은 개체의 Show Time. 위치를 정수 픽셀로 자르지 않는다. 텍스트 박스는 폭 `w - 8` 에서 줄바꿈하므로 **크롤 박스 폭이 글자 길이보다 작으면 끝부분이 둘째 줄로 밀려 보이지 않는다**(박스 폭은 글자 폭에 여유를 두어 정한다). |
| 진단 | `--jsprof` 일 때 1초마다 `[jstat]`(rAF 호출·간격, `frame()`/`render()` 소요, GC 추정)와 텍스트가 박스(w×h)에 들어가는지 점검 결과를 로그에 남긴다. 평소엔 꺼짐. |

## 8. 글꼴

| 항목 | 정책 |
|---|---|
| 쓰는 글꼴 | `bin/fonts` 의 파일만 쓴다(.ttf .otf .woff .woff2). 파일 이름이 글꼴 이름이다. `-Bold`/`-Italic`/`-BoldItalic` 접미사는 같은 글꼴의 굵게/기울임으로 묶고, Light/Medium/Black 은 `NotoSansKR-Light` 처럼 별개 글꼴로 등록한다(`SemiBold`/`ExtraBold` 이름은 끝이 `Bold` 라 잘못 묶이므로 쓰지 않는다). `bin/fonts/fonts.css` 는 폴더를 읽어 자동으로 만든다(송출 런타임이 이 파일로 등록). |
| 포함 폰트 | 모두 SIL OFL 1.1(라이선스 전문은 `bin/fonts/licenses`). 고딕: NanumGothic, **NotoSansKR**(Light/Regular/Medium/Bold/Black), **Pretendard**(Light/Regular/Medium/Bold/Black), **SpoqaHanSansNeo**(Light/Regular/Medium/Bold, 한자 제외 서브셋), GothicA1, IBMPlexSansKR, GowunDodum. 명조: NanumMyeongjo, GowunBatang. 제목·장식: BlackHanSans, DoHyeon, Jua, Gugi, YeonSung. 손글씨: NanumPenScript, Gaegu, PoorStory, HiMelody. |
| 윈도우 전용 폰트 | 맑은 고딕·Arial·돋움·바탕·궁서·Georgia·Times New Roman 등은 단말(Debian)에 없고 배포도 못 하므로 쓰지 않는다. 송출 런타임의 기본 글꼴은 `NotoSansKR`. |
| 옛 글꼴 이름 치환 | 송출 런타임이 프로젝트를 읽을 때(`fixItem`) 아래 규칙으로 바꾼다(글자 일부 서식 `runs` 포함). 맑은 고딕·Malgun Gothic·Arial·Helvetica → NotoSansKR, Arial Black → NotoSansKR-Black, 돋움·Dotum → NanumGothic, 바탕·Batang·궁서·Gungsuh·Georgia·Times New Roman → NanumMyeongjo. |

## 9. 프로젝트 파일(`bin/project/*.json`)

| 항목 | 정책 |
|---|---|
| 송출 프로젝트 선택 | `start.sh` 는 `CG_PROJECT` 환경변수, 없으면 `.run/last-project`(마지막으로 적용한 프로젝트), 둘 다 없으면 빈 프로젝트로 시작한다. |
| 예제 | `자막예제-*`(crawl-right/top/2line, roll-up/down, lowerthird, headline-pages, fold-center/blind/3lines)는 HDMI 영상 개체 + 투명 배경 구성이다. 크롤 박스 폭은 글자 폭(`NotoSansKR-Bold`)에 맞춰 정하고 한 번 흐르는 시간은 (화면 폭 + 박스 폭) / 속도이다. 상하로 접히는 형태는 `scale` 효과의 `effectPreset=5`(세로만 줄임)로 만든다. |

## 10. 인코더와 송출

| 항목 | 정책 |
|---|---|
| 해상도 | 1920×1080 고정. |
| 비디오 인코더 | MPP 하드웨어 H.264 High, CBR 8Mbps(상수 `kBitrate`, 하한·상한 각 ±1/16), GOP = 반올림한 fps(IDR 1초 간격), B 프레임 없음. 색 변환·합성은 RGA. |
| TS | MPEG-TS, 패킷마다 즉시 전송(`AVFMT_FLAG_FLUSH_PACKETS`), PAT/PMT 0.1초 주기 재전송, PCR 오프셋(`max_delay`) 0.1초, `pkt_size=1316`. |
| 출력 대상 | `output=1` 로컬 HDMI 만(전체화면, 인코딩 없음) / `2` UDP 만(기본) / `3` HDMI + UDP(인코딩 결과를 미리보기 창에도 표시). `udp_ip`·`udp_port` 는 `cgsetup.cfg`, 환경변수 `CG_UDP` 가 임시로 덮어쓴다. |
| 인코더가 늦을 때 | 틱을 건너뛰고(`miss`) 프레임이 아직 없으면 그 틱은 출력하지 않는다. |

## 11. 로그

| 항목 | 정책 |
|---|---|
| 파일 | `bin/log/cg-streamer-YYYY-MM-DD.log`, 줄마다 `YYYY-MM-DD HH:MM:SS.mmm` 시간, 일별 파일, 30일 보관. stdout·stderr 와 CEF 자식 프로세스 출력이 같은 파일로 모인다. tty 로 직접 실행하면 콘솔에 그대로 출력하고, `CG_LOG_STDOUT=1` 이면 파일을 쓰지 않는다. 로거 시작 전 출력은 `log/cg-streamer.out`. |
| 1초 통계 | `[estat]` 인코딩 루프(`enc`, `paint`, `gap_max`, `late25`, `big100`, `op_avg/op_max`(OnPaint 소요), `out`, `rga`, `video`, `uiq`, `under`, `drop`, `miss`, `accel_fail`), `[pstat]` 미리보기 창(`drawn`, `skipped_x`). 접두어는 7자로 폭을 맞춘다. |
| 입력 timestamp | `[hdmirx-ts]` 영상·음성 timestamp 가 지금보다 얼마나 과거인지(1초). |

## 12. 알려진 한계

- **밀린 틱을 따라잡지 않는다.** 만료 횟수가 1보다 커도 한 번만 처리한다.
- **muxer 는 PTS 를 보정하지 않는다.** 되감김·중복 방지는 HDMI 영상 입력 쪽(`직전 + 1`)에만 있고 음성과 틱 시각 PTS 에는 없다. PTS 가 시스템 가동 시간이라 TS 33비트(약 26.5시간, 95,443.7초)에서 되감긴다. muxer 는 하위 33비트만 써서 자동으로 잘라 넣고, **ffmpeg 수신기는 되감김을 끊김 없이 이어 붙인다**(`correct_ts_overflow`: TS 를 33비트로 선언하고 첫 타임스탬프 60초 앞을 기준으로 ADD/SUB 방향을 정해 이후 값을 보정, 경계 근처에서 시작한 TS 로 확인). 그 밖의 수신기(VLC, GStreamer, 하드웨어 디코더 등)가 받아들이는지는 확인하지 않았다. **송출을 26.5시간 넘게 연속으로 돌려 경계를 실제로 통과하는 시험(26.5시간 시험)이 필요하다**(수신 단말별로, 또는 경계 근처에서 시작하는 TS 를 해당 단말로 보내 미리 확인).
- **CG 만 있을 때의 PTS 는 틱 시각**이다. CEF 가 그림을 보낸 시각(`OnPaint` 입력 시각)은 PTS 에 쓰지 않는다.
- **영상 파일의 PTS 를 쓰지 않는다.** 가변 fps 파일이나 PTS 가 끊기는 파일에서는 어긋날 수 있다.
- **라이브 음성이 직접 송출되는 동안 믹서의 다른 소리(영상 파일 음성, 효과음)는 송출되지 않는다**(로컬 재생에는 들린다).
- **입력과 틱의 시계 차이.** HDMI 입력 59.94 에 `fps=60` 이면 약 16초마다, `fps=59.94` 이면 입력 소스 오차(약 28ppm)만큼(약 10분에 한 번) 프레임이 반복되거나 건너뛴다. 입력 프레임 도착을 틱으로 쓰는 방식은 구현하지 않았다.
- **CEF 는 못 그리면 `paint` 가 60 아래로 떨어지고** 그만큼 직전 그림이 반복된다(`under`). 렌더링이 무거운 내용(정적 텍스트가 많은 페이지 등)에서 일어나며 7절의 텍스트 캐시로 줄였지만 초당 0~1회의 놓침은 남는다.
- **글자 폭 차이.** 단말에 없는 글꼴은 다른 글꼴로 대체되어 박스에 안 들어갈 수 있다. 8절의 글꼴만 쓰면 PC 와 단말이 같다.
- 인코더 비트레이트(8Mbps)는 상수이고 설정으로 바꿀 수 없다. 인코더가 못 따라갈 때를 위한 인코더 분할·재정렬은 이 저장소에 구현되어 있지 않다.
