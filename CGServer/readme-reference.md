# 참고 프로그램 분석: FFmpeg(fps 필터/x11grab), Sunshine

작성일: 2026-10-05
목적: 틱/타임스탬프 정책(`readme-policy.md`)의 열린 항목 — 밀린 틱을 어떻게 채울지(P6), 늦을 때 격자를 어떻게 다룰지(P4) — 를 실제 프로그램에서 확인.
방법: 각 저장소 `master` 브랜치 원문을 내려받아 직접 읽음(2026-10-05 시점). 읽은 범위는 각 절에 명시.

## 1. 요약

| 프로그램 | 대기(페이싱) | 늦을 때 | 영상 PTS | 새 프레임이 없을 때 |
|---|---|---|---|---|
| GStreamer `videotestsrc` | 번호에서 절대 시각 계산 | 격자 유지, 연속 따라잡기 | 번호 기반(격자) | 해당 없음 (합성 소스) |
| GStreamer `ximagesrc` | 시각에서 번호 역산 | 번호 건너뜀 | 정시는 격자, 늦으면 실제 시각 | 칸당 최대 1프레임 |
| **FFmpeg `x11grab` (`xcbgrab`)** | `time_frame += duration` 누적, 리셋 없음 | 대기 없이 연속 캡처로 따라잡음 | **실제 시각** (`av_gettime()`) | 하류 `fps` 필터가 처리 |
| **FFmpeg `fps` 필터 / `-vsync cfr`** | (필터, 대기 없음) | 입력 PTS 와 출력 격자의 차이로 중복/드롭 결정 | 출력 카운터(`next_pts++`) | 직전 프레임 복제 |
| **Sunshine** | `next_frame += delay`, 1프레임 넘게 밀리면 `now + delay` 로 리셋 | 격자를 현재로 이동(틱 소실) | 인코딩한 프레임 수(`frame_nr++`) | 최소 fps 타임아웃 시 직전 프레임 재인코딩 |
| 현재 cg-streamer | `next += period`, 1주기 넘게 밀리면 `next = tick_now` | 격자 이동(Sunshine 과 동일) | Write 횟수(`n_`) | 직전 NV12 버퍼 재인코딩 |

핵심 발견: **현재 코드의 틱 처리(누적 + 리셋)는 Sunshine 과 사실상 같은 패턴**이다. 반면 FFmpeg `xcbgrab` 은 리셋 없이 누적하고, GStreamer 는 번호 계산으로 격자를 고정한다. 실제 프로그램도 갈린다.

## 2. FFmpeg `x11grab` (`libavdevice/xcbgrab.c`) — 읽은 범위: `wait_frame`, `xcbgrab_read_packet`, 초기화

```c
// 초기화 (:618)
c->frame_duration = rescale(1, 1/fps → 마이크로초);
c->time_frame     = av_gettime_relative();          // 시작 시각

// wait_frame (:204-218)
c->time_frame += c->frame_duration;                  // 누적, 번호 계산 아님
for (;;) {
    curtime = av_gettime_relative();
    delay   = c->time_frame - curtime;
    if (delay <= 0) break;                           // 이미 지났으면 바로 탈출
    av_usleep(delay);
}

// read_packet (:426-475)
wait_frame(s, pkt);
pts = av_gettime();                                  // 대기 후의 실제 벽시계 시각
... 화면 캡처 ...
pkt->dts = pkt->pts = pts;                           // PTS = 실제 시각 (마이크로초)
pkt->duration = c->frame_duration;
```
- **대기 시각은 누적**(`time_frame += duration`)이지만 **리셋이 없다.** 늦으면 `delay <= 0` 이라 잠들지 않고 곧바로 캡처하며, 다음 호출에서도 `time_frame` 은 격자를 따라 한 칸씩만 전진하므로 밀린 칸 수만큼 **연속 캡처**한다(GStreamer `videotestsrc` 와 같은 따라잡기).
- 단, **PTS 는 격자가 아니라 실제 캡처 시각**(`av_gettime()`)이다. 연속 캡처된 프레임들의 PTS 는 서로 아주 가깝다. 정시 프레임은 격자 간격에 가깝다.
- CFR 맞춤은 이 장치(demuxer) 몫이 아니라 하류의 `fps` 필터(아래 3절)가 한다. **"실제 시각을 PTS 로 주고, 중복/드롭은 다른 단계가 한다"는 역할 분리**다.
- 타임스탬프 단위는 마이크로초(`avpriv_set_pts_info(st, 64, 1, 1000000)`, :590).

## 3. FFmpeg CFR 변환 — `fps` 필터와 `video_sync_process`

읽은 범위: `libavfilter/vf_fps.c`의 `write_frame`/`shift_frame`, `fftools/ffmpeg_filter.c`의 `video_sync_process`(:2660-2760).

### 3-1. `fps` 필터 (`vf_fps.c:266-326`) — "홀드" 규칙
```c
// 입력 프레임을 2개까지 버퍼링. next_pts 는 출력 카운터.
if (frames_count == 2 && frames[1]->pts <= next_pts) {
    drop frames[0];                       // 두 번째 입력이 이미 이번 출력 칸에 쓸 수 있으면 첫 번째는 버림
} else {
    out = clone(frames[0]);               // 아니면 첫 번째 프레임을 이번 칸에 출력(복제)
    out->pts = next_pts++;                // 출력 PTS 는 카운터
    cur_frame_out++;
}
```
- **출력 PTS 는 카운터(`next_pts++`)** 라서 항상 연속이다.
- 출력 칸 `k` 에는 **"PTS 가 `k` 이하인 가장 최근 입력"** 을 쓴다. 새 입력이 아직 없으면 같은 프레임을 다시 복제한다(= 중복). 입력이 칸보다 촘촘하면 중간 것을 버린다(= 드롭).
- GStreamer `videorate` 가 "가장 가까운 쪽"을 고르는 것과 달리, 이 필터는 **"지나지 않은 마지막 프레임을 유지"** 하는 규칙이다.
- 통계: `frames_in`, `frames_out`, `dup`, `drop` 카운터와 로그("Duplicated frame ... N times", "Dropping frame").
- `start_time` 옵션으로 첫 PTS 를 지정하고, `round`(기본 `near`)로 반올림 방식을 정한다. `eof_action` 으로 마지막 프레임 처리를 정한다.

### 3-2. `-vsync cfr` 의 판정 (`ffmpeg_filter.c` `video_sync_process`, :2660-2760)
```c
delta0 = sync_ipts - next_pts;            // 입력 프레임이 출력 격자에서 얼마나 벗어났는지 (프레임 단위)
delta  = delta0 + duration;

if (delta0 < 0 && delta > 0 && vsync != PASSTHROUGH) {   // 약간 과거로 시작한 프레임은 자름
    sync_ipts = next_pts;  duration += delta0;  delta0 = 0;
}
// CFR:
if (frame_drop_threshold && delta < frame_drop_threshold && frame_number) nb_frames = 0;
else if (delta < -1.1)      nb_frames = 0;                 // 많이 앞섬 → 드롭
else if (delta >  1.1) {                                   // 많이 뒤처짐 → 중복
    nb_frames = llrintf(delta);
    if (delta0 > 1.1) nb_frames_prev = llrintf(delta0 - 0.6);   // 이전 프레임도 반복
}
// 그 외(-1.1 ~ 1.1): 정확히 1프레임 출력
```
- **±1.1프레임의 불감대**: 입력이 격자에서 1.1프레임 이내면 그대로 1프레임 출력한다. 지터로 중복/드롭이 흔들리는 것을 막는 히스테리시스다.
- 많이 뒤처졌을 때 `nb_frames = round(delta)` 만큼 한꺼번에 반복하고, 이전 프레임을 몇 번 반복할지(`nb_frames_prev`)도 따로 정한다.
- **복제 폭주 방지 상한**: `nb_frames > dts_error_threshold × 30` 이면 "frame duplication too large, skipping"(:2739). `dts_error_threshold` 기본값이 `3600*30` 이라(ffmpeg_opt.c:57) 상한이 약 324만 프레임으로 **사실상 제한이 없다.**
- `frame_drop_threshold` 기본 0 이면 해당 분기는 꺼져 있다.

## 4. Sunshine (LizardByte) — 읽은 범위: `video.cpp` 인코딩 루프, `platform/linux/x11grab.cpp` 캡처 루프, `platform/linux/misc.h` `handle_pacing`

### 4-1. 캡처 페이싱 (`platform/linux/misc.h:118-131`)
```cpp
inline void handle_pacing(time_point &next_frame, nanoseconds delay, auto &logger) {
  auto now = steady_clock::now();
  if (next_frame > now) {
    std::this_thread::sleep_until(next_frame);        // 아직 이르면 절대 시각까지 대기
    logger.first_point(next_frame); logger.second_point_now_and_log();   // 오버슈트 측정
  }
  next_frame += delay;                                // 누적
  if (next_frame < now) {                             // "some major slowdown happened; we couldn't keep up"
    next_frame = now + delay;                         // 격자를 현재로 이동
  }
}
```
- **현재 cg-streamer 의 EncodeLoop 와 같은 패턴**이다: `sleep_until` + 누적 + 뒤처지면 현재 시각으로 리셋.
- 차이: Sunshine 은 `next_frame += delay` 를 한 뒤 `< now` 이면(즉 한 주기 넘게 밀리면) 리셋한다. 우리는 `tick_now - next >= period` 로 비슷한 조건이다.
- **`sleep_until` 의 오버슈트를 측정해 주기적으로 로그**한다(`sleep_overshoot_logger`, `"Frame capture sleep overshoot"`). 틱 지터를 계속 관찰하는 장치다. 우리가 `tick_late_max` 를 추가하려는 것과 같은 목적.
- `platf::high_precision_timer`(`timerfd` 기반이라고 주석에 적혀 있으나, Linux 구현 `sleep_for` 는 `std::this_thread::sleep_for` 를 호출한다, `platform/linux/misc.cpp:1554-1555`)는 별도의 정밀 타이머 추상화다. 이 경로의 실제 사용처는 읽지 않았다.

### 4-2. 캡처 루프 (`x11grab.cpp:562-600`)
```cpp
while (true) {
  platf::handle_pacing(next_frame, delay, sleep_overshoot_logger);
  status = snapshot(pull_free_image_cb, img_out, 1000ms, cursor);
  switch (status) {
    case timeout: push_captured_image_cb(img_out, /*frame_captured=*/false); break;   // 새 화면 없음 알림
    case ok:      push_captured_image_cb(img_out, /*frame_captured=*/true);  break;
  }
}
```
- 캡처는 고정 간격으로 하고, **새 화면이 없으면 "캡처 안 됨(false)"으로 통지**한다. 이미지에는 캡처 시각(`img->frame_timestamp = steady_clock::now()`)을 붙인다(:620, :777).

### 4-3. 인코딩 루프 (`video.cpp:2435-2510`) — 이미지 도착 구동 + 최소 fps 타임아웃
```cpp
// 최소 fps 목표 (기본: 요청 fps / 2)
minimum_fps_target = (config.minimum_fps_target > 0) ? config.minimum_fps_target : config.framerate / 2;
max_frametime = 1000 / minimum_fps_target;            // ms

while (true) {
  ...
  // "정지 화면에서도 최소 fps 로 인코딩해 화질 문제를 피한다"
  if (auto img = images->pop(max_frametime)) {        // 새 이미지를 max_frametime 까지 기다림
      session->convert(*img);
  } else if (!images->running()) break;               // 타임아웃이면 convert 없이 그대로 아래로
  ...
  encode(frame_nr++, ...);                            // frame->pts = frame_nr  (:1830)
}
```
- **인코더는 틱이 아니라 이미지 도착으로 구동**된다. 캡처 스레드가 고정 간격으로 이미지를 밀어 넣고, 인코딩 루프는 도착할 때마다 인코딩한다.
- 이미지가 `max_frametime`(기본 1/(fps/2) 초 = 60fps 요청 시 약 33ms) 안에 오지 않으면 `pop` 이 타임아웃되어 **변환 없이 직전 프레임을 그대로 다시 인코딩**한다. 즉 **중복 프레임은 "최소 fps 하한"에서 생기고**, 그 사이에는 이미지가 오는 대로 가변 간격으로 인코딩한다(완전한 CFR 이 아니다).
- **PTS 는 `frame_nr++`** — 인코딩한 프레임 수다(`encode_avcodec`: `frame->pts = frame_nr`, :1830). 현재 cg-streamer 의 `n_`(Write 횟수)와 같은 방식이다.
- 시작 시 **더미 이미지**를 미리 변환해 두어 첫 프레임을 기다리다 타임아웃돼도 인코딩할 것이 있게 한다(:2447-2457). 우리의 `!cur.seq` → `continue` 처리와 대비된다.
- 인코더 설정: B-프레임을 쓰지 않고(`:2053`), 저지연 구성(GOP 무한, low delay, `:1172`).

주의(해석의 한계): Sunshine 은 Moonlight 클라이언트로 **실시간 재생**하는 게임 스트리밍이다. 이 절에서는 영상 쪽 경로만 읽었고, 오디오 타임스탬프와 클라이언트의 동기 방식은 읽지 않았다. 따라서 "`frame_nr` 카운터 PTS 가 문제없다"는 것을 TS 로 PTS 기반 A/V 동기를 하는 이 프로젝트에 그대로 적용할 수는 없다.

## 5. 정책에 주는 시사점

### 5-1. P4 (격자를 옮기지 않는다) — 실제 프로그램도 갈린다
| 방식 | 사용처 |
|---|---|
| 격자 유지(번호 계산 또는 누적·리셋 없음) | GStreamer `videotestsrc`, FFmpeg `xcbgrab` |
| 리셋(격자 이동) | Sunshine, 현재 cg-streamer |
- P4 는 GStreamer 와 FFmpeg 쪽 방식이다. Sunshine 이 리셋을 쓰는 이유(주석: "major slowdown ... couldn't keep up")는 **"밀린 틱을 몰아서 따라잡는 것이 오히려 해로울 수 있다"** 는 판단이다. 실시간 게임 스트리밍은 지연이 쌓이는 것보다 프레임을 버리는 쪽이 낫다. P5(연속 따라잡기)에는 같은 위험이 있다. `readme-gstreamer.md` 9절과 이전 점검에서 지적한 "따라잡기 역효과" 문제다.
- 해석: P4/P5 를 유지하더라도 **따라잡기 폭주를 막는 장치**(예: 연속 처리할 최대 틱 수, 임계 초과 시 이동)에 대한 별도 결정이 필요한지 판단해야 한다. 이는 정책 변경이므로 사용자 결정 사항이다.

### 5-2. P6 (밀린 틱은 직전 그림 중복) — 규칙의 구체화에 FFmpeg `fps` 필터가 가장 직접적인 참고
- FFmpeg 의 규칙: 칸 `k` 에는 **"PTS 가 `k` 이하인 최신 입력"** 을 쓴다. 우리 구조에서는 "그 틱에 새 UI 그림이 도착했으면 그것, 아니면 직전 그림"과 같다(이미 EncodeLoop 가 하는 동작).
- 지터 억제용 불감대(±1.1프레임, `video_sync_process`)의 개념은 우리 지터 버퍼(5장)가 이미 하는 역할과 겹친다.
- **중복 상한**: FFmpeg 은 이론상 상한이 있으나 기본값으로 사실상 무제한이다. 폭주 가능성을 별도로 다룰지는 미정.

### 5-3. 측정 도구 — Sunshine 의 `sleep_overshoot_logger`
- `sleep_until` 이 목표 시각을 얼마나 넘겨서 깨어나는지를 주기적으로 로그한다. 우리의 `tick_late_max`/`tick_late_cnt` 와 같은 아이디어이고, **이미 실서비스에서 쓰는 방식**이다.

### 5-4. 인코더 구동 방식 — 틱 구동 vs 이미지 구동
- Sunshine: 이미지 도착 구동 + 최소 fps 타임아웃 → 가변 간격(VFR 성격).
- cg-streamer: 고정 틱 구동 → 항상 틱마다 1프레임(CFR).
- 이 프로젝트가 UDP TS 로 PTS 를 쓰는 점을 고려하면 틱 구동(CFR)이 적합하다는 것이 현재 정책(P6)과 일치한다. Sunshine 방식(이미지 구동)을 채택하자는 근거는 이번 분석에서 발견되지 않았다.

## 6. 읽지 못한 부분 / 한계
- FFmpeg: `x11grab` 구 구현(`libavdevice/x11grab.c`)은 내려받기에서 비어 있어(0줄) 읽지 못함. 현행 `xcbgrab.c` 만 읽음. `ffmpeg_enc.c` 에는 vsync 코드가 없고 `ffmpeg_filter.c` 로 옮겨져 있음을 확인했다.
- FFmpeg `fps` 필터는 입력 2프레임 버퍼 로직만 읽었고 EOF 처리 세부는 읽지 않음.
- Sunshine: `kmsgrab.cpp` 의 캡처 루프도 같은 `handle_pacing` 을 쓰는 것을 grep 으로만 확인(전체 읽지 않음). 오디오 경로, 클라이언트 동기, `capture_frame_interval` 정의는 읽지 않음.
- 세 프로그램 모두 **소스 코드만** 읽었고 실제 실행해서 틱 지터나 A/V 어긋남을 측정하지는 않았다.
