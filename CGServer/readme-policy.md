# 정책서

기준일: 2026-10-05. "현재 구현" 열은 이 날짜 단말(Debian 12, cg-streamer)에서 확인한 상태이다. 정책과 다르면 ⚠ 로 표시한다.

| # | 항목 | 정책 | 현재 구현 |
|---|---|---|---|
| 1 | 전송 | fps tick 을 이용해서 전송한다. fps tick 은 `timerfd` 를 이용하여 구현한다. | ✔ `EncodeLoop`(`main.cpp`)가 `timerfd_create(CLOCK_MONOTONIC)` + `read()` 로 틱을 만든다. 이전의 `sleep_until` 은 제거. |
| 2 | 송출 PTS | 송출의 PTS 는 입력 시의 timestamp 로 설정한다. | ✔ muxer(`TsMuxer`)는 **timestamp(ns)를 90kHz 로 환산만** 한다. 프레임 번호·기준점(시간 0) 빼기·단조 보정 없음. 영상·음성 모두 `CLOCK_MONOTONIC` 이라 PTS 값이 곧 시스템 가동 시간이다. timestamp 가 없는 프레임(CG만, 영상 파일, 같은 입력을 다시 쓰는 틱)은 틱 시각을 쓴다. |
| 3 | 기준 소스 | 기준은 미디어/라이브 소스이다. 합성 프레임의 PTS 는 미디어/라이브 소스의 timestamp 를 기준으로 한다. | △ 라이브(HDMI RX)는 그 프레임의 V4L2 timestamp 가 PTS. **미디어 파일은 아직 파일 timestamp 가 아니라 틱 시각**(정책 6 참고). |
| 4 | 늦은 tick | `timerfd` 가 늦게 깨어나 만료 횟수가 1보다 크면, 그 횟수만큼 따라잡는다. | ⚠ **미구현.** 만료 횟수가 1보다 커도 한 번만 처리하고, 밀린 횟수만 `[estat]` 의 `miss=` 로 센다(`TODO(정책 4)`). 따라잡을 때 각 틱에 쓸 입력이 정해지지 않았기 때문. |
| 5 | 라이브 음성 PTS | 라이브 음성의 PTS 는 ALSA 캡처 시각으로 한다. | ✔ ALSA 를 tstamp 모드 ENABLE + 타입 `MONOTONIC` 으로 설정하고, `snd_pcm_status_get_tstamp` 의 µs 값을 **샘플 수 보정 없이 그대로** PTS 로 쓴다. 믹서를 거치지 않고 1024샘플 블록으로 모아 muxer 에 직접 보낸다(`WriteLiveDirect`). |
| 6 | 미디어 파일 PTS | 미디어 재생 파일이 있을 때는 재생 파일의 비디오·오디오 PTS 를 사용한다. | ⚠ **미구현.** 영상 파일 재생 시 영상 PTS = 틱 시각, 음성 = 믹서(`PushAt`, 재생 예정 시각) 블록 시작 시각이다. 파일의 PTS 는 쓰지 않는다. 그래도 싱크는 맞는 것을 사용자가 확인함(같은 `CLOCK_MONOTONIC` 축). |
| 7 | CG만 있는 구간 | 기준 소스(미디어/라이브)가 없으면 CEF 를 기준으로 한다. PTS 는 CEF `OnPaint` 입력 시각으로 한다. CEF 는 화면 변화가 없어도 60fps 로 계속 `OnPaint` 를 보내므로(단말 측정: 정지 화면에서 `paint=60/s`) 정지 화면에서도 입력이 끊기지 않는다. | ⚠ **정책과 다름.** 지금은 틱 시각이다(CEF `OnPaint` 입력 시각을 PTS 로 쓰는 코드는 없음). 그림은 지터 버퍼(5장)를 거쳐 틱마다 1장씩 꺼내므로, 입력 시각을 PTS 로 쓰면 도착 지터가 PTS 에 그대로 실려 지터 버퍼의 평활 효과와 충돌한다. |

## 현재 구현 상세 (2026-10-05)

- **시계**: 영상 V4L2 timestamp, ALSA tstamp, `timerfd`, `std::steady_clock` 모두 `CLOCK_MONOTONIC`(부팅 후 경과 시간, 시각을 바꿔도 점프하지 않음).
- **라이브 영상**(`hdmirx_source.cpp`): `VIDIOC_DQBUF` 의 `buf.timestamp` 를 µs(`tv_sec*1000000 + tv_usec`)로 사용하고, 직전 값 이하면 직전 + 1 로 올린다(`last_pts_us_`, 재연결해도 유지). `V4L2_BUF_FLAG_ERROR` 버퍼는 다시 큐에 넣고 버린다. timestamp 가 `MONOTONIC` 이 아니면 도착 시각으로 대체한다(이 보드는 `MONOTONIC` 으로 확인, age 0.1ms).
- **같은 라이브 프레임을 틱 2번에 쓰는 경우**(60Hz 틱 vs 입력 위상): 두 번째부터는 입력 timestamp 가 아니라 틱 시각을 PTS 로 쓴다(`main.cpp` `last_live_ts`).
- **믹서 경로**(영상 파일 음성, 효과음, 로컬 HDMI 재생용): 블록(1024샘플) 시작 시각(격자)이 PTS. 라이브 음성이 직접 송출 중(마지막 호출 후 300ms)이면 믹서의 송출은 건너뛴다.
- **PTS 간격**: 입력 timestamp 그대로라 **간격은 일정하지 않다**(영상 2~33ms, 음성 7~32ms). 간격이 일정할 필요는 없다는 것이 사용자 결정.
- **확인된 것**: 라이브 입력, 영상 파일 재생 모두 비디오/오디오 싱크 일치(사용자 확인). 라이브 음성을 믹서로 보낼 때 있던 잡음은 직접 송출 후 사라짐.
- **송출 fps**: `cgsetup.cfg` 의 `fps=60`. CEF `windowless_frame_rate` 도 같은 값이지만 이것은 **상한**이다(CEF 문서). CEF 가 16.7ms 안에 못 그리면 `paint` 가 낮아지고, 송출은 60 틱이라 직전 그림이 반복된다. `생활정보-문자방송` 은 정적 텍스트를 매 프레임 다시 그리느라 `paint` 가 33~40/s 였으나(바탕색·순수 CG 여부와 무관), `cg-runtime.js` 의 텍스트 캐시와 캐시 미리 생성으로 평균 59.7/s 가 되었다. 남은 끊김은 초당 0~1회(CEF/GPU 스케줄링으로 추정, 미확인). `--paint-fps=30` 고정과 외부 BeginFrame 은 시험했지만 더 불규칙해서 쓰지 않는다(readme-history.md 2026-10-05 7)).
