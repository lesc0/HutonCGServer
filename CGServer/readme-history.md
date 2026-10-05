# 작업 히스토리 (Debian 12)

Debian 11 시절 기록은 `readme-history-d11.md` 참고.

## 2026-10-05

### 1) 60fps 틱 조사 — CEF/인코더 틱의 성격, 다른 CG 서버(CasparCG·OBS·vMix)와 비교 (코드 수정 없음)

#### 현재 구조
- 인코딩 틱은 `EncodeLoop`(`main.cpp`)의 `sleep_until(next)`(소프트웨어 타이머, `next += period` 절대 시각 누적). 한 주기 이상 늦으면 `next = tick_now` 로 리셋(밀린 틱을 몰아 실행하지 않음).
- CEF 틱은 `windowless_frame_rate`(CEF 내부 합성 타이머). `--gpu --cef:use-angle=gles-egl` 로 GPU 렌더링을 해도 틱 소스가 HW vsync 로 바뀌지는 않는다(OSR 에는 디스플레이 vsync 가 없고 Chromium 의 DelayBasedTimeSource 계열 타이머로 동작 — 문서 기반 추정, CEF 소스는 직접 확인 못 함). GPU 는 그리는 시간을 짧고 일정하게 해 `paint=60/s` 가 맞게 해 줄 뿐.
- 두 시계(CEF 60Hz / EncodeLoop 60Hz)가 별개라 박자가 어긋나는 것을 지터 버퍼(`kUiPrime`=5)가 흡수.

#### 이 단말에서 쓸 수 있는 HW 시계 후보 (현재 틱에는 미사용)
- DRM vblank(HDMI 출력): `drm_out.cpp` 의 `drmModeAtomicCommit` 이 이미 다음 vblank 까지 블록. HDMI 출력이 켜졌을 때(output=1/3)만 가능, UDP 단독(output=2)이면 없음.
- HDMI-RX 프레임 도착(`hdmirx_source.cpp` 의 `VIDIOC_DQBUF`): 입력 신호가 있을 때만.
- ALSA 오디오 클럭(48kHz, 60fps=800샘플).
- 주의: HW 틱의 실제 주파수가 60.000Hz 와 다르면(59.94 등) `TsMuxer` 의 카운터 PTS 와 어긋나 수신 버퍼가 서서히 차거나 빔.

#### 다른 CG 서버의 틱 (소스/포럼 확인)
- **CasparCG**: 채널 루프는 `produce → mix → output_()` 이고 속도는 `output_()` 에서 결정. 소비자 중 `has_synchronization_clock()` 이 true 인 것(예: DeckLink)이 있으면 그 소비자의 `send()` 블록이 틱이 되고, 모두 false(예: ffmpeg 소비자)면 `sleep_until(time_ += 1e6/hz)` 로 자체 타이머 사용. 루프 스레드는 실시간 우선순위. → HW 시계는 HW 출력이 있을 때만, 없으면 `sleep_until` (현재 구현과 동일).
- **OBS**: `video_sleep()` 의 `os_sleepto_ns()`(리눅스는 `nanosleep` 상대 시간) 로 다음 목표 시각까지 잠. vsync/HW 시계 없음.
- **vMix**: 소스 비공개. 포럼상 내부 free-run 시계, 외부 genlock 미지원(동기가 필요하면 별도 싱크 제너레이터를 I/O 카드에 연결).
- 결론: 방송용 SW 도 HW 출력이 없으면 SW 타이머로 틱을 만든다. UDP 단독 구성에서 현재 `sleep_until` 은 업계 일반 수준.

#### 중요: OBS 의 "늦은 틱" 처리 vs 현재 코드 (`obs-video.c` `video_sleep`)
```c
t = cur_time + interval_ns;
if (os_sleepto_ns(t)) { *p_time = t; count = 1; }
else {                                   // 이미 t 가 지남 = 늦음
  count = max(diff, interval_ns) / interval_ns;   // 지난 프레임 수
  *p_time = cur_time + interval_ns * count;       // 시계를 count 칸 앞으로 (격자 위상 유지)
}
lagged_frames += count - 1;  vframe_info.count = count;
```
- 렌더는 **한 번만** 하고(틱을 count 번 실행하지 않음), 시계는 `interval × count` 만큼 앞으로 가므로 원래 격자 위에 남는다. 그 프레임에 `count` 를 실어 인코더가 **같은 프레임을 count 번 복제**(`queue_frame` 의 `duplicate`)해 PTS 가 연속(CFR) 유지.
- 현재 코드(`main.cpp` 556행 부근): `if (tick_now - next >= period) next = tick_now;` → 격자 위상이 현재 시각으로 이동. 더구나 `TsMuxer` 의 영상 PTS 는 프레임 카운터(`n_`)라 늦은 틱으로 건너뛴 시간이 타임라인에서 사라지고, 오디오 PTS 는 `base_` 기준 실시간이라 **늦은 틱마다 A/V 가 한 프레임씩 어긋날 수 있다**(누적 여부는 미확인). 참고: `[stat]` 의 `enc` 가 55~60fps 로 떨어지는 구간이 있었음(위 2026-10-04 2) 항목).
- 적용 방안(미적용) — OBS 방식을 EncodeLoop 에 옮기면:
  1. 늦으면 `late = (tick_now - next) / period` 를 구하고 `next += period * late` 로 격자를 유지한다.
  2. 인코딩은 한 번만 한다.
  3. PTS 를 `n_ += 1 + late` 로 올리거나(VFR, PTS 에 구멍), 건너뛴 만큼 직전 프레임을 `late` 번 재인코딩해서 PTS 를 이어 붙인다(CFR). OBS 는 후자인 복제 방식.
  - 둘 중 무엇이 나은지는 단말 수신 측(UDP TS 를 받는 쪽)이 VFR 을 받아들이는지에 달림 → 결정 필요.
  - 선행 작업: `late` 횟수를 `[stat]` 에 찍어 실제 발생 빈도를 먼저 본다.
- 참고: OBS 의 `nanosleep` 은 상대 시간이라 호출 사이 지연이 오차에 들어감. 현재의 `sleep_until` 이 이 점에서는 유리할 것으로 추정(libstdc++ 구현은 미확인).
- ※ 이 항목에서 말한 `sleep_until` 틱과 프레임 카운터(`n_`) PTS 는 아래 3), 5) 에서 각각 `timerfd` 와 입력 timestamp 로 바뀌었다.

### 2) 단말 CEF OnPaint 60fps 측정 (코드 수정 없음)

- 측정 도구: 이미 있던 `[stat]` 의 `paint/s`, `gap_max`, `late25`(25ms 넘게 늦은 횟수), `big100`, `enc`, `uiq`, `drop`.
- **CG 만 송출(정지 화면), 56초**: `paint` 평균 59.77/s(58~61, 범위 이탈 0초), `enc` 평균 60.01, `late25` 초당 약 2회, `gap_max` 최대 39ms, `big100` 0, `drop` 0, `uiq` 1~2.
  - 개수는 60fps 로 일정하지만 간격은 균일하지 않다. 매초 1~2번 한 프레임씩 늦게 오고 지터 버퍼가 흡수해 출력 `enc` 는 60.0.
- **정지 화면에서도 `OnPaint` 는 60/s 로 계속 온다**(출력 0.09Mbps 인 거의 변화 없는 화면에서도). 그래서 `paint` 값만으로는 실제 애니메이션이 60fps 로 그려지는지 알 수 없다(움직이는 요소로 따로 봐야 함).
- **틱의 정체**: CEF 문서(`cef_types.h`)에 `windowless_frame_rate` 는 "OnPaint 가 호출되는 **최대** fps(1~60, 기본 30), 못 만들면 더 낮아질 수 있음"으로 적혀 있고 vsync/EGL swap 언급은 없다. `paint` 가 정확히 60 이 아니라 59.8±2 로 흔들리는 것도 SW 타이머와 맞다 → **HW(EGL/디스플레이) 틱이 아니라 CEF 내부 타이머**로 판단. 단 내부 타이머 클래스는 CEF 소스를 보지 못해 추정. 직접 증거가 될 `--paint-fps=30` 실험은 스크립트가 `[stat]` 를 못 잡아 **미완**.
- 어제(Debian 12, 4K HDMI 합성) 로그 825초 집계: `paint` 평균 59.84(55~64), `enc` 평균 59.92, `late25` 초당 1.34, `drop` 17(전부 HDMI 입력 구간).
- 정책서 반영: 정책 7(CG 만 있는 구간은 CEF `OnPaint` 입력 시각 기준), 미결정 12 ☑.

### 3) 송출 fps 틱을 `timerfd` 로 변경 (`main.cpp` EncodeLoop)

- `sleep_until(next)` → `timerfd_create(CLOCK_MONOTONIC)` + `read()`. 만료 횟수가 1 보다 크면 밀린 틱 수를 `[stat]` 의 `miss=` 로 센다. 이전의 "한 주기 이상 늦으면 `next = tick_now` 리셋" 코드는 삭제(timerfd 는 절대 격자로 만료).
- **밀린 틱 따라잡기(정책 4)는 미구현**(`TODO(정책 4)`): 한 번만 처리하고 건너뛴다(이전 동작과 같음). 정책서 미결정 3(따라잡기 틱에 쓸 입력) 결정 후 구현.
- 결과: CG 만 송출 시 `enc` 60.0, `miss=0~1`.

### 4) 로그를 시간 표시 일별 파일로 (`log_writer.cpp`)

- `--run` 시작 시 stdout/stderr 를 파이프로 받아, 줄마다 `YYYY-MM-DD HH:MM:SS.mmm` 을 붙여 `bin/log/cg-streamer-YYYY-MM-DD.log` 에 기록. 자정에 새 파일, 30일 지난 파일은 새 파일을 열 때 삭제. CEF 자식 프로세스 출력도 같은 파일로 모인다.
- tty 로 직접 실행하면 콘솔 그대로(켜지 않음). `CG_LOG_STDOUT=1` 로도 끌 수 있다.
- `start.sh` 의 리다이렉트는 `log/cg-streamer.out` 으로 변경(로거 시작 전 출력용, 예: `mpp_platform: client 18 driver is not ready!`). 기존 `log/cg-streamer.log` 는 더 이상 갱신되지 않는다.
- 단말 빌드는 `-Werror` 라 `snprintf` 버퍼(16바이트)가 `format-truncation` 으로 실패 → 64바이트로 수정. 단말에서 로거만 따로 컴파일해 줄 단위 시간 표시, stderr/자식 출력, 줄 조각 합치기, 오래된 파일 삭제를 확인(자정 전환은 미확인).

### 5) 라이브 입력 PTS 를 V4L2/ALSA timestamp 로 (프레임 번호 폐기)

#### 구조
- **영상**: V4L2 `VIDIOC_DQBUF` 의 `buf.timestamp` 를 µs(`tv_sec*1000000 + tv_usec`)로 PTS 원천으로 쓰고, 직전 값 이하면 직전 + 1(단조 증가, `last_pts_us_` 로 재연결 후에도 유지). `V4L2_BUF_FLAG_ERROR` 버퍼는 다시 큐에 넣고 버린다. timestamp 가 `MONOTONIC` 이 아니면 도착 시각으로 대체(이 보드는 MONOTONIC 으로 확인, age 0.1ms).
- **음성**: ALSA 를 `sw_params` 로 tstamp 모드 ENABLE + 타입 MONOTONIC 으로 설정하고(시계를 영상과 맞춤), `snd_pcm_status_get_tstamp` 의 µs 를 **샘플 수 보정 없이 그대로** PTS 로 사용. 믹서를 거치지 않고 1024샘플 블록으로 모아 `AudioMixer::WriteLiveDirect` → `TsMuxer::WriteAudioNs` 로 직접 송출. 직접 송출 중(마지막 호출 후 300ms)에는 믹서의 송출을 건너뛴다. 로컬 재생용으로 믹서에는 계속 넣음.
- **muxer**: PTS = timestamp(ns)를 90kHz 로 **환산만** 한다(기준점 빼기·단조 보정 없음). 영상·음성 모두 `CLOCK_MONOTONIC` 이라 PTS 값이 곧 시스템 가동 시간. timestamp 가 없는 프레임(CG 만, 같은 입력을 다시 쓰는 틱)은 틱 시각(`CLOCK_MONOTONIC`)을 쓴다. 프레임 번호(`n_`)는 폐기.
- 시계 설명: `CLOCK_MONOTONIC` = 부팅 후 경과 시간(시각을 바꿔도 점프하지 않음). `std::steady_clock`, `timerfd(CLOCK_MONOTONIC)`, V4L2 MONOTONIC timestamp, ALSA MONOTONIC tstamp 가 모두 같은 시계.

#### 시행착오
1. 음성을 처음엔 `htstamp` 에서 `(읽은 n + avail)` 샘플을 빼 **블록 첫 샘플 시각**으로 보정했다(age 평균 34ms). 사용자 지시로 `snd_pcm_status_get_tstamp` 그대로(보정 없음, age 0.0ms)로 변경.
2. 이 값은 믹서(`PushLive` → 링에 배치 → 블록 시작 시각이 PTS)를 거치면 `kLiveLatency`(0.12초)와 1024샘플 격자가 섞여 "ALSA timestamp 가 그대로 PTS" 가 되지 않는다 → 라이브 음성만 믹서를 우회하도록 변경.
3. `-Werror` 로 `tick_now` 미사용 경고가 CG_EBF 가 아닌 빌드에서 실패 → EBF 블록 안으로 이동.

#### 측정 (8초 캡처, HDMI 4K 입력)
- 영상 PTS 481개 모두 증가(간격 평균 16.65ms, 최소 1~2ms, 최대 33ms). 음성 PTS 377개 모두 증가, 간격은 평균 21.33ms 이지만 7~32ms 로 **불규칙**(status 를 읽은 시각이 그대로 PTS 라서 정상. 간격이 일정할 필요는 없음).
- V4L2 timestamp 진단: age 평균 0.0~0.1ms, 프레임 간격 최대 16.7ms(60Hz 규칙적). muxer 경고 없음. 시작 시 `Packets poorly interleaved, failed to avoid negative timestamp` 가 한 번 나오는 것은 이전에도 있었음(AAC 인코더 지연).
- **사용자 확인: 비디오/오디오 싱크 일치.** 이전(믹서 경로)에 있던 오디오 잡음이 직접 송출 후 없어짐(믹서의 `live_pos_` 재동기로 샘플이 겹치거나 비던 것이 원인일 가능성, 미확인). 오디오 `kLiveLatency` 는 라이브 직접 송출에는 쓰이지 않는다.
- **사용자 확인: 영상 파일(mp4) 재생 중에도 싱크 일치.** 이 경로는 영상 PTS=틱 시각(`CLOCK_MONOTONIC`), 음성=믹서(`PushAt`, 재생 예정 시각)이며 `kLiveLatency` 는 쓰지 않는다. 영상 PTS 만 프레임 번호에서 틱 시각으로 바뀌었는데도 두 PTS 가 같은 시계 위라 맞는 것으로 보인다.

#### 남은 문제 / 결정 필요
- 같은 시도에서 `enc` 가 51~58fps, `drop`/`miss` 가 1분에 수백까지 쌓이는 때가 있었고(재시작마다 편차 큼, `miss=0` 인 때도 있음) 위 2026-10-04 의 Debian 12 + 4K HDMI 증상과 같은 패턴. 음성 변경과의 상관은 불명(이전 커밋과 같은 조건 비교 미실시).
- 라이브 직접 송출 중에는 믹서의 다른 소리(영상 파일 음성, 효과음)가 **송출되지 않는다**(로컬 재생에는 들림). 로컬 HDMI 재생 쪽 잡음 여부는 미확인.
- CG 만 송출할 때의 PTS 는 지금 틱 시각. 정책 7(CEF `OnPaint` 입력 시각)과 다름 → 어느 쪽으로 할지 결정 필요(OnPaint 도착 간격이 곧 PTS 지터가 되어 지터 버퍼의 평활 효과와 충돌).
- PTS 가 시스템 가동 시간이라 TS 33비트(약 26.5시간)에서 되감김. muxer 가 처리하지만 수신 단말이 받아들이는지는 확인한 적 없음(정책서 미결정 11).
- 밀린 틱 따라잡기(정책 4) 미구현(3) 참고).

### 6) 로그 접두어 변경: `[stat]` → `[estat]`, `[preview-stat]` → `[pstat]`

- 두 접두어 폭이 달라(`[stat]` 6자 / `[preview-stat]` 14자) 값 열이 어긋났다. 처음엔 `[stat]` 뒤에 공백을 채워 폭을 맞췄다가, 사용자 요청으로 짧은 이름 `[estat]`(encode) / `[pstat]`(preview) 7자로 통일(공백 패딩 제거). `--no-encode` 의 `[stat] no-encode` 도 `[estat]`.
- 위 2)~5) 항목의 `[stat]` 는 당시 이름 그대로 두었다. `grep` 은 `\[estat\]`, `\[pstat\]` 로. 지금은 `[hdmirx-ts]` 만 폭이 다르다(미정렬).

### 7) 생활정보 문자방송 가로 스크롤 끊김 조사와 개선 (CEF `paint` 34 → 59.7/s)

#### 증상과 첫 측정
- `생활정보-문자방송`(24개 아이템, 크롤 1개) 의 자막 스크롤이 `가로스크롤-예제` 보다 많이 툭툭거린다. 단말 `[estat]`: `paint` 33~40/s(30 으로 일정하지도 않음), `under`(직전 그림 반복) 초당 약 15씩 증가, `enc` 는 60.0 유지, `drop`/`miss` 0.
- CEF 의 `windowless_frame_rate=60` 은 **상한**이라(CEF 문서) CEF 가 못 만들면 `paint` 가 낮아지고, 송출은 60 틱이라 부족한 만큼 직전 그림이 반복된다(화면은 약 35fps 로 보임).

#### 원인 찾기 (단말에서 프로젝트 변형을 돌려 `paint` 비교)
| 변형 | `paint`/초 |
|---|---|
| `자막프로젝트`(정지, 아이템 2) / 불투명 바탕(`#0a1a38`)·전체 화면 `바탕` 사각형을 넣어도 | 59~60 |
| `생활정보` / 바탕을 투명으로 / 전체 화면 `바탕` 사각형 제거 | 약 36 (그대로) |
| `가로스크롤-예제`(아이템 5, HDMI 영상 포함) / 영상 제거(순수 CG) | 59.8 / 59.7 |
| `생활정보` + HDMI 영상 아이템 추가 | 33.5 |
| `생활정보` 에서 **크롤 외 텍스트 전부 제거** | **59.5** |
| `생활정보` 에서 여러 줄 "내용" 박스 4개만 제거 | 46.9 |
- 바탕색이 원인이 아님(사용자 가설 확인: 아님), 순수 CG 라서도 아님. 원인은 **정적 텍스트를 매 프레임 다시 그리는 비용**. `cg-runtime.js` 는 매 프레임 `clearRect` 후 모든 아이템을 다시 그리는데, 크롤/롤 텍스트만 오프스크린 캔버스에 캐시하고 나머지 텍스트·시계는 `drawText`(글자마다 `fillText`)로 매번 그렸다. 생활정보는 정적 텍스트 12개(40px, 여러 줄 4개 포함).
- 사각형 10개는 모두 단색(그라데이션·줄무늬·모서리·그림자·테두리 없음)이라 캐시해도 이득이 없어 하지 않음.

#### 수정 (`bin/web/cg-runtime.js`)
1. **정적 텍스트/시계도 오프스크린 캔버스에 캐시**: `effect` 가 `text`(글자가 점점 나타남)가 아니고 그림자가 없으면 `cachedTextCanvas` 로 한 번만 그리고 매 프레임 `drawImage`. 폰트가 늦게 로드되면 캐시를 버리도록 `document.fonts` 의 `loadingdone` 마다 `fontEpoch` 를 올려 키에 포함. → `paint` 평균 34 → 58.9.
2. **프로젝트 로드 직후 모든 페이지의 텍스트 캐시를 미리 생성**(`prewarmTextCache`, 첫 로드와 reload 둘 다): 페이지가 바뀌는 순간 그 페이지 글자를 처음 그리느라 `render()` 가 131ms 걸리던 튐 제거. → `render()` 최대 131ms → 12ms.

#### 측정 도구 (진단용, 평소엔 꺼짐)
- `cg-streamer --jsprof`(player.html `?jsprof=1`): 렌더러에서 1초마다 `[jstat]` 를 로그에 남김 — rAF 호출 수·`dt_max`·`late`(33ms 벌어진 횟수), `frame()`/`render()` 평균·최대, `slow8`(8ms 초과), `gc`(JS 힙이 0.5MB 넘게 줄어든 횟수), `heap`. `cefQuery` 의 `prof:` 메시지로 전달(`main.cpp` `OnQuery`).
- `[estat]` 에 `op_avg`/`op_max` 추가: `OnPaint` 한 번에 걸리는 시간(8MB 복사·락 포함).
- 송출 영상으로 크롤 부드러움 측정: PC 에서 UDP 를 받아 `ffmpeg` 로 크롤 띠(y=1004~1060)를 잘라 프레임별 열 프로파일의 상호상관으로 이동량(px/프레임)을 구해, 정지(0)·건너뜀(2배) 프레임을 센다. TS 연속성(CC) 오류로 UDP 손실이 없음을 먼저 확인.

#### 시도했지만 효과 없던 것
- **`--paint-fps=30` 고정**: 새 그림이 나오는 간격이 2틱(정상)인 비율 54%(1틱 199, 3틱 142, 4틱 33)로 오히려 불규칙. CEF 30Hz 타이머와 송출 60Hz 틱의 위상이 어긋나고, 30 모드는 지터 버퍼를 안 쓰고 "최신 그림" 방식이라 흡수 못 함.
- **외부 BeginFrame(`-DENABLE_EXTERNAL_BEGIN_FRAME=ON`, `CG_EBF`)**: 60 은 `paint` 51~60(평균 약 56)에 고립 정지 프레임 5.8% 로 기존(0.8%)보다 나쁨, 30 은 정상 간격 67%. 별도 빌드 디렉터리에서 시험하고 삭제(설치본은 건드리지 않음).

#### 병목이 아닌 것으로 확인된 것 (`--jsprof`, `op_*`)
- JS: `frame()` 평균 1.3ms(예산 16.7ms 의 8%), GC 0(힙 9.5MB 로 일정). `OnPaint` 평균 2.4~4ms·최대 5~8ms 라 다음 프레임을 막지 못함.
- 남은 "가끔 툭": rAF 가 33ms 로 벌어지는 일이 초당 0~1회. JS 가 일하고 있지 않은 시점이라 CEF/GPU 프로세스·스레드 스케줄링(CPU 경쟁, 8코어에서 렌더러·GPU·브라우저와 우리 SCHED_FIFO 스레드)으로 추정하나 미확인.

#### 결과
- 생활정보 `paint` 평균 **34 → 59.7/s**(`under` 초당 15 → 25초에 약 10, `late25` 초당 13~22 → 2~6). 송출 영상의 크롤은 이동 프레임 대부분이 일정하게 5px(이론 4.66)씩 이동하고, 고립 정지 프레임은 약 0.8%(40초에 18개, 약 2초에 1번). 사용자 확인: 좋아졌으나 "가끔 툭툭"은 남음.

### 8) 인코더 fps 를 분수로 (29.97 / 30 / 59.94 / 60)

- 배경: CEF 의 `windowless_frame_rate` 는 `int`(1~60, `cef_types.h`)라 59.94 를 줄 수 없다. 그래서 **인코더 쪽만** 분수로 받고 CEF 는 정수 근사(30/60)를 쓴다.
- `cgsetup.cfg` 의 `fps=` 를 `ParseFps` 로 분수로 변환(정수 N/1, NTSC 계열 N×1000/1001, 그 밖의 소수 ×1000/1000). `g_fps_num/g_fps_den`(정확한 값)과 `g_fps`(반올림, CEF 페인트·통계 주기·지터 버퍼 조건용)를 분리.
- 적용: MPP rate control `rc:fps_in/out_num/denorm`(GOP 는 반올림 fps), `TsMuxer::Open(fps_num, fps_den)`(`avg_frame_rate`, 패킷 duration), `EncodeLoop` 틱 주기 `1e9 × den / num` ns. 영상 PTS 는 입력 timestamp 그대로라 계산식 변화 없음.
- 단말 확인(임시 cfg, 15초 캡처): `59.94` → 스트림 `60000/1001`, 실측 59.940fps(901프레임/15.015초), PTS 간격 1501/1502 교대(이상값 1501.5). `29.97` → `30000/1001`, 실측 29.970fps(450프레임/14.982초), PTS 간격 3003 중심.
- 시행착오: 빌드 중 오류 메시지 문자열을 생성 스크립트의 `chr(92)` 로 만들다가 C++ 소스에 그대로 들어가 컴파일 실패 → 수정. `bin/cgsetup.cfg` 는 단말에 로컬 수정이 있어 주석은 건드리지 않고 `readme-dev.md` 에 설명.
- 미확인: 라이브 HDMI 입력(59.94 입력)에서 설정을 59.94 로 맞췄을 때 프레임 반복·건너뜀이 줄어드는지.

### 9) 개발 보조

- 단말 SSH 공개키 등록: `bin/setup-ssh-key.ps1`(Windows PowerShell, 키 생성/등록/접속 확인). 한글이 깨지지 않도록 UTF-8 **BOM** 으로 저장. `bin/*` 가 gitignore 라 저장소에는 올라가지 않는다(올리려면 `.gitignore` 에 예외 추가).
- 임시로 PC 로 UDP 송출: `CG_UDP=<ip>:1234 bin/start.sh`. PC 에서 `UdpClient(1234)` 로 받아 TS 동기 바이트(0x47)와 `ffprobe -show_entries packet=pts` 로 PTS 를 확인했다.

## 2026-10-04

### 1) 단말 OS 를 Debian 12 로 변경 — 소스 구성과 컴파일 환경 구성

#### 배경
- 테스트 단말(CM3588, 10.10.10.56)의 OS 를 Debian 11 → **Debian 12 (bookworm) arm64** 로 교체. 작업 경로는 `/root/work/github.cgserver` 에서 `/home/pi/work/github.cgserver` 로 변경. 최종 소스 백업은 `/userdata/github.cgserver_20261004.tgz`.
- OS 재설치로 단말 호스트 키가 바뀌어 PC 의 `known_hosts` 에서 `10.10.10.56` 항목을 지우고 다시 등록.

#### 소스 구성
- GitHub 를 `/home/pi/work/github.cgserver` 에 clone. 최종 소스 tgz 는 `/home/pi/work/final` 에 풀어 비교 → `src` 는 GitHub 와 동일, 최종본에만 CEF 런타임(libcef.so 등)·미디어·`lib/` 가 더 있음.
- 최종본의 `bin/` 런타임 파일을 clone 의 `bin/` 으로 복사(`cp -an`). `lib/`(Debian 11 용 벤더 라이브러리)는 쓰지 않고 Debian 12 시스템 패키지를 사용.

#### 컴파일 환경
- apt 설치: build-essential, cmake 3.25, ninja, pkg-config, libx11/xext/xrandr/gtk-3/drm/asound 개발 패키지, ffmpeg 5.1 개발 패키지, node 18/npm 등. MPP·RGA 개발 패키지(`librockchip-mpp-dev`, `librga-dev`)는 이미 시스템에 있어 그대로 사용. Debian 12 의 cmake 3.25 가 프리셋 요구(3.21)를 만족해 Kitware 별도 설치는 불필요.
- **CEF 는 새로 빌드하지 않고** 최종 소스의 `libcef.so` 를 링크: `/opt/cef/cef_custom_130_arm64`, `~/.bashrc` 에 `CEF_ROOT` 등록.
- 커스텀 CEF tar(`doc/cef/cef_custom_130_arm64.tar.gz`)에 컴파일용 파일이 빠져 있어 같은 버전(130.1.16) 공식 배포본에서 **CMake 파일과 헤더만** 보충(바이너리는 미사용):
  - `cmake/`, `CMakeLists.txt`, `libcef_dll/CMakeLists.txt`
  - `include/` 전체 교체(`cef_version.h`, `cef_config.h`, `cef_net_error_list.h` 가 없거나 소스 트리용이라 `net/base/net_error_list.h` 를 찾지 못함)
  - `chrome-sandbox`, `libvulkan.so.1`, pak, `icudtl.dat`, `locales` 는 최종 소스 `bin/` 에서 복사(빌드의 post-link 복사 단계가 요구)
- 공식 배포본(약 470MB)은 HTTP/2 로 중간에 끊겨 `curl --http1.1 -C -` 로 이어받음.

#### 소스 수정
- `video_source.cpp`: `#include <libavcodec/bsf.h>` 추가. ffmpeg 5.x 는 `avcodec.h` 에 `AVBSFContext`·`av_bsf_*` 선언이 없어 컴파일 오류가 났음.

#### 결과
- `cd /home/pi/work/github.cgserver/CGServer/src/cg-streamer && ./build.sh install` 로 컴파일·링크 성공, `ldd` 누락 라이브러리 없음, `bin/cg-streamer` 에 설치. (실행 `start.sh` 는 아직 확인 전)
- 개발환경 정보는 `readme-dev.md` 에 정리.
- 미완: `cg-editor`(node) 의 `npm install`/빌드는 하지 않음.

### 2) 단말 실행 확인과 Node.js 22 설치

#### 실행
- `DISPLAY=:0 CG_SKIP_KIOSK=1 bin/start.sh` 로 실행: cg-streamer 정상 기동(프로젝트 `생활정보-문자방송`, MPP H.264 1080p60 CBR 8Mbps 초기화 확인). UDP 수신·화면은 미확인.
- cg-editor 는 `node_modules` 가 없어 시작 실패. 실시간 우선순위(`ulimit -r`=0)로 SCHED_FIFO 미사용(limits.d 설정 필요).

#### Node.js
- cg-editor 가 Node `>=22.13` 을 요구하는데 apt 의 nodejs 는 18.20 → apt 패키지 제거 후 공식 바이너리 **v22.23.3**(npm 10.9.9)을 `/opt/node` 에 설치하고 `/usr/local/bin` 에 링크. 체크섬(SHASUMS256) 확인.
- 설치 중 링크 대상 경로가 비어(`/opt/node//bin/node`) 한 번 잘못 만들어져 다시 링크함.
- 설치 방법은 `readme-dev.md` 의 "Node.js" 참고.

#### 실시간 우선순위(rtprio) 설정
- `/etc/security/limits.d/99-cg-rt.conf` 에 `pi - rtprio 90` 추가(`pam_limits` 는 sshd 에 이미 켜져 있음). 새 로그인부터 `ulimit -r`=90, 재시작한 cg-streamer 프로세스의 `Max realtime priority`=90 확인 → start.sh 의 "ulimit -r 이 0" 안내 사라짐.
- 단, 엔진이 SCHED_FIFO 를 실제로 쓰는지는 `bin/cgsetup.cfg` 의 `realtime=on` 에 달림(현재 `off` 로 두어 스레드는 모두 일반 스케줄링 TS). 켜려면 cfg 를 바꾸고 재시작.
- 데스크톱(lightdm) 세션에서 띄우는 프로세스에는 그 세션을 다시 로그인해야 적용된다.
- 참고: 원격 명령 줄에 `cg-streamer --run` 문자열이 들어가면 start.sh 의 `pgrep -f` 가 실행 중으로 오인하므로 스크립트 파일로 나눠 실행할 것.

#### realtime=on 적용, 성능 확인, 화면 잠금 해제
- 보드 `bin/cgsetup.cfg` 를 `realtime=on` 으로 바꿔 재시작 → FIFO 스레드 7개(cg-encode·mpp_h264e 50, cg-preview 49, cg-audio·cg-hdmirx·cg-hdmi-aud 48, cg-audio-out 47) 확인. (보드 로컬 변경, 커밋 안 함)
- GPU 모드 확인: `--gpu --cef:use-angle=gles-egl`, gpu-process 동작, paint=60/s.
- `drop` 이 초당 약 1.4장 누적(4K HDMI 입력 합성 중, uiq 7~8, enc 55~60fps). 과거 Debian 11 기록은 `drop 0~1/5.5분`. 코드상 UI 지터 버퍼에서 버리는 것이며 원인 조사 중.
- HDMI 에 아무것도 안 나옴: 모니터가 HDMI-2 로 바뀐 뒤 `light-locker` 화면 잠금으로 데스크톱 세션이 비활성(로그인 화면)이 되어 `cg-preview` 창이 가려짐 → `sudo loginctl unlock-session 1 && sudo loginctl activate 1` 로 복구.
- 화면 잠금/절전 해제: light-locker·xscreensaver 자동 시작 끄기, xfce4-power-manager 의 DPMS 등 끄기, 로그인 때 `xset s off/-dpms` 적용(자세한 내용은 `readme-dev.md`).

#### cg-editor 빌드와 실행
- `src/cg-editor/build.sh`(`npm ci` + Next.js 빌드)가 Node 22 로 성공 → `.next/` 생성.
- `bin/start.sh` 로 cg-editor(prod, 포트 8080) 기동, `http://localhost:8080/` 응답 200. cg-streamer 는 계속 실행 중. 키오스크는 생략(`CG_SKIP_KIOSK=1`).
