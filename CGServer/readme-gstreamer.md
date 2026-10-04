# GStreamer 라이브 소스의 틱/타임스탬프 방식 분석과 cg-streamer 적용 설계

작성일: 2026-10-05
목적: EncodeLoop 의 60fps 틱(`sleep_until` 누적 + 늦으면 `next = tick_now` 리셋)을 GStreamer 방식(PTS 기반 절대 시각 대기)으로 바꾸기 위한 사전 분석.

- 분석 대상: GStreamer `main` 브랜치 원문을 직접 내려받아 읽음(2026-10-05 시점).
  - `subprojects/gstreamer/libs/gst/base/gstbasesrc.c` (4301줄)
  - `subprojects/gstreamer/gst/gstsystemclock.c` (1574줄)
  - `subprojects/gstreamer/gst/gstclock.c` (1778줄)
  - `subprojects/gstreamer/gst/gstpipeline.c` (1245줄)
  - `subprojects/gstreamer/libs/gst/base/gstbasesink.c` (6021줄)
  - `subprojects/gst-plugins-base/gst/videotestsrc/gstvideotestsrc.c`
  - `subprojects/gst-plugins-base/gst-libs/gst/audio/gstaudiobasesrc.c` (1248줄, `create()` 의 타임스탬프 부분)
  - `subprojects/gst-plugins-base/gst/videorate/gstvideorate.c` (2404줄, 입출력 선택 로직 부분)
  - `subprojects/gst-plugins-base/gst-libs/gst/audio/gstaudioclock.c` (전체)
  - `subprojects/gstreamer/gst/gstbin.c` (latency 관련 부분), `subprojects/gstreamer/meson.build` (futex 검사)
  - `subprojects/gst-plugins-good/sys/ximage/gstximagesrc.c` (`create()`), `sys/v4l2/gstv4l2src.c` (`create()`/latency query), `sys/v4l2/gstv4l2bufferpool.c` (타임스탬프 부분)
  - 내려받았지만 읽지 않은 것: `gstpushsrc.c` (QoS 참조 grep 만)
- 아래 "확인" 은 원문 코드를 직접 읽은 것, "미확인" 은 읽지 못했거나 추정인 것.

---

## 1. 전체 구조: 소스 스레드 하나가 "만들고 → 기다리고 → 밀어낸다"

`gst_base_src_loop()` (gstbasesrc.c:2885) 가 소스의 스트리밍 스레드 루프이고, 한 바퀴가 버퍼 1개다.

```
gst_base_src_loop
  └ gst_base_src_get_range                    (:2563)
       ├ create()  ← 서브클래스가 프레임 생성 (videotestsrc 는 fill())
       ├ gst_base_src_do_sync(buffer)         (:2715)  ← 여기서 시계 대기 + 타임스탬프 보정
       └ 결과(status) 처리
  └ gst_pad_push(buffer)                      ← 다운스트림으로 전달
```

- 별도의 "틱 타이머" 가 없다. **프레임을 만든 직후, 그 프레임의 PTS 에 해당하는 절대 시각까지 `gst_clock_id_wait` 로 잠드는 것**이 곧 틱이다.
- 이 점이 현재 EncodeLoop(주기를 누적해 `sleep_until` → 깨어나서 프레임 생성)와 순서가 반대다. GStreamer 는 "프레임에 시각이 붙고, 그 시각까지 기다린다".

## 2. 타임스탬프(PTS)는 카운터에서 계산한다 (확인: gstvideotestsrc.c:1334-1410)

```c
pts = accum_rtime + timestamp_offset + running_time;
...
n_frames++;
next_time = gst_util_uint64_scale(n_frames, fps_d * GST_SECOND, fps_n);
GST_BUFFER_DURATION(buffer) = next_time - running_time;
running_time = next_time;
```

- `running_time` 은 `period` 를 더해 가는 것이 아니라 **매번 `n_frames × fps_d / fps_n` 을 정수 스케일로 새로 계산**한다. 59.94(=60000/1001) 같은 분수 fps 에서도 반올림 오차가 쌓이지 않는다.
- `duration` 은 인접한 두 PTS 의 차이라서 프레임마다 16ms/17ms 로 달라질 수 있지만 합은 정확하다.
- 이 프로젝트의 `TsMuxer::Write` 도 `av_rescale_q(n_, {1,fps_}, time_base)` 로 같은 방식이다(`ts_muxer.cpp:103`). **PTS 계산 방식 자체는 이미 같다.** 차이는 `n_` 이 무엇을 세느냐(아래 7절)와 대기 시각이 어디서 오느냐다.

## 3. `gst_base_src_do_sync` — 대기 시각은 PTS 에 시작 오프셋을 더해 만든다 (확인: gstbasesrc.c:2299-2460)

```c
// get_times: 라이브이면 start = PTS, end = PTS + duration   (videotestsrc: gstvideotestsrc.c:1211-1230)

// 첫 버퍼에서 딱 한 번
now          = gst_clock_get_time(clock);
running_time = now - base_time;
ts_offset    = running_time - timestamp;       // (pseudo_live && timestamp 유효일 때)

// 모든 버퍼 (라이브)
PTS   += ts_offset;
DTS   += ts_offset;
start += ts_offset;

gst_base_src_wait(basesrc, clock, start + base_time);   // 절대 시각 대기
```

핵심 성질:
1. **`ts_offset` 은 첫 프레임에서 한 번만 정하고 고정**한다. 이후 모든 프레임의 대기 시각은 `base_time + ts_offset + PTS_n` 이라, "시작 시각 + n번째 이상적 시각" 의 고정 격자 위에 놓인다.
2. 어떤 프레임이 늦게 깨어나도 **다음 프레임의 목표 시각은 격자 위 그대로**다. 지연이 누적되지도, 격자 위상이 이동하지도 않는다.
3. PTS 도 같은 `ts_offset` 이 더해져 **시계의 running time 과 같은 축**에 놓인다. 즉 "PTS = 그 프레임이 나갔어야 할 시계 시각".
4. 첫 버퍼 처리 시 `latency` 도 계산한다: `timestamp <= start` 이면 `latency = start - timestamp` (gstbasesrc.c:2333-2350). 파이프라인이 라이브 지연을 알 수 있게 하려는 것.

## 4. 늦으면 어떻게 하나 — 소스는 버리지도 건너뛰지도 않는다 (확인: gstbasesrc.c:2718-2725, gstsystemclock.c:1300)

```c
case GST_CLOCK_EARLY:
  /* the buffer is too late. We currently don't drop the buffer. */
  break;
```

- 목표 시각이 이미 지났으면 `gst_clock_id_wait` 가 **즉시 `GST_CLOCK_EARLY` 로 반환**한다(잠들지 않음). 소스는 그대로 버퍼를 내보내고 곧바로 다음 프레임을 만든다.
- 다음 프레임의 목표 시각도 격자 위(이미 지났거나 곧 도래)이므로, **밀린 만큼 연속으로 빠르게 만들어 따라잡는다.** 프레임 수는 줄지 않는다(소스에서는 드롭 없음).
- 반환값에는 지연량(jitter = `-diff`)도 들어간다(gstsystemclock.c:1170 `*jitter = -diff`).
- **드롭 판단은 소스가 아니라 싱크의 몫**이다. `gst_base_sink_is_too_late()` (gstbasesink.c:3167): `status == EARLY` 이고 `max_lateness != -1` 일 때만, `rstart + jitter > rstop + max_lateness` 이면 늦은 프레임을 버린다. 그리고 **1초 넘게 아무것도 렌더하지 못하면 늦었어도 강제 렌더**("emergency", 같은 함수)해서 화면이 멈춘 것처럼 보이지 않게 한다.

OBS 와의 비교:
| | 늦을 때 시계 | 프레임 처리 |
|---|---|---|
| OBS (`video_sleep`) | `time += interval × count` (격자 유지) | 렌더 1회, 그 프레임을 `count` 번 복제 |
| GStreamer 소스 | 격자 유지 (PTS 기반) | 프레임을 빼먹지 않고 연속 생성으로 따라잡음 |
| 현재 코드 | `next = tick_now` (**격자 이동**) | 틱 1개 소멸, PTS(`n_`)는 그대로 이어져 시간이 사라짐 |

## 5. 시스템 시계의 대기 구현 (확인: gstsystemclock.c:1142-1310)

`gst_system_clock_id_wait_jitter_unlocked()`:

```c
now     = gst_clock_get_time(clock);        // CLOCK_MONOTONIC 기반 (gstsystemclock.c:167, :1042)
mono_ts = g_get_monotonic_time();
diff    = entry_time - now;                 // 기다려야 할 시간 (음수면 이미 늦음)
jitter  = -diff;

if (diff > CLOCK_MIN_WAIT_TIME) {
  loop {
    if (diff <= 500us) {
      clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &end, NULL);   // 정밀 대기
    } else {
      if (diff < 2ms) diff -= 500us;        // 끝 500us 는 정밀 대기로 남겨 둠
      pthread_cond_timedwait(... mono_ts + diff ...);                 // 거친 대기 (CLOCK_MONOTONIC cond)
    }
    // 깨어난 뒤 now 를 다시 읽어 diff 재계산
    if (diff <= CLOCK_MIN_WAIT_TIME) { status = OK; done; }
    else 다시 대기 (restart)
  }
} else {
  status = (diff == 0) ? OK : EARLY;        // 이미 지났거나 거의 도래
}
```

- **2단계 대기**: 먼저 조건변수 `timedwait` 로 (목표 − 0.5ms) 까지 거칠게 기다리고, 마지막 500µs 이하는 **`clock_nanosleep(TIMER_ABSTIME)`** 로 절대 시각 정밀 대기한다.
- 깨어날 때마다 현재 시각을 다시 읽어 **남은 시간을 재계산**한다(조기 기상/조건변수 시그널 대비).
- `CLOCK_MIN_WAIT_TIME` (확인: gstsystemclock.c:220, :394, :522): 대기 구현 3가지에 따라 값이 다르다. futex 구현 **100ns**, pthread 구현 **500ns**, GCond 구현은 Windows 1ms / 그 외 1µs(주석 기준). 이 값 이하로 남았으면 잠들지 않고 바로 반환한다. 선택 규칙(확인): meson 이 `linux/futex.h` 와 `futex` syscall 을 쓰는 코드를 컴파일해 보고 되면 `HAVE_FUTEX`(또는 `HAVE_FUTEX_TIME64`)를 켠다(gst meson.build:312-329). 따라서 **일반적인 Linux 빌드는 futex 구현(100ns)** 을 쓴다. 배포판(Debian 12) 패키지가 이 옵션으로 빌드됐는지는 확인하지 못했다. `time64` 가 있으면 먼저 시도하고 `ENOSYS` 이면 `futex` 로 재시도한다(gstsystemclock.c:254-281).
- **시계 값은 되감기지 않는다** (확인: gstclock.c `gst_clock_adjust_unlocked`): `priv->last_time = MAX(ret, priv->last_time)`. 보정(calibration: internal/external 기준점과 rate 분수)을 적용한 값이 이전보다 작아지면 이전 값을 유지한다. 시스템 시계의 내부 시각은 `CLOCK_MONOTONIC` (gstsystemclock.c:167).
- 이 구현은 이 프로젝트의 `sleep_until(next)`(C++ `steady_clock`)와 본질이 같다(둘 다 CLOCK_MONOTONIC 절대 시각 대기). **대기 함수 자체를 바꿀 이유는 없고, "무엇을 목표 시각으로 삼느냐"가 차이**다. 다만 대기 후 재확인 루프는 참고할 만하다.

## 6. 파이프라인 지연(latency)과 오디오 시계

- `videotestsrc` 의 라이브 지연 질의 응답 (확인: gstvideotestsrc.c:1149-1165): `min = 1프레임(fps_d/fps_n 초)`, `max = NONE`, `live = 라이브 여부`. 라이브 소스는 "최소 한 프레임은 늦게 나간다" 고 알린다. 싱크는 `min + processing_deadline + render_delay` 를 합산해 재생 시각을 늦춘다(gstbasesink.c:1270-1356).
- 오디오 소스의 시계 제공과 타임스탬프 방식은 6-4 에서 자세히 다룬다.
- 요점: GStreamer 파이프라인은 **시계 하나가 영상/음성의 공통 시간축**이고, 모든 PTS 는 그 시계의 running time(= `now − base_time`)에 맞춰진다.

## 6-1. `base_time` 은 어떻게 정해지나 — 시간축의 0점 (확인: gstpipeline.c:398-495)

```c
// PLAYING 으로 갈 때 파이프라인이 한 번 수행
clock = gst_element_provide_clock(pipeline);     // 시계 선택
now   = gst_clock_get_time(clock);
new_base_time = now - start_time + delay;        // :474
gst_element_set_base_time(pipeline, new_base_time);   // 모든 하위 요소에 배포
```
- `running_time = clock_time − base_time`. 즉 `base_time` 은 **"running_time 0 에 해당하는 시계 시각"** 이다. 모든 요소가 같은 `base_time` 을 받으므로, 영상 소스·오디오 소스·싱크가 같은 시간축에 놓인다.
- `start_time` 은 "첫 버퍼가 가져야 할 running time"(보통 0, flush/PAUSED 때 리셋: `reset_start_time`, :312). `delay` 는 파이프라인 속성으로 "요소들이 기동하는 데 걸리는 시간 여유"를 더한다. **기본값은 0** (확인: gstpipeline.c:100 `DEFAULT_DELAY 0`).
- 시계가 바뀌었거나 running time 이 리셋됐을 때만(`update_clock || last_start_time != start_time`) `base_time` 을 새로 잡고, 아니면 **기존 `base_time` 을 유지**한다(:484-489). 즉 한 번 정한 0점은 흔들리지 않는다.
- 이 프로젝트에 대응시키면: 공통 `t0` 가 곧 `base_time` 이다. 영상 틱 `n` 의 시각 = `t0 + scale(n)`, 오디오 블록 `n` 의 시각 = `t0 + n×1024/48000`.

## 6-2. 파이프라인 latency 배분 (확인: gstpipeline.c:682-770)

```c
query latency → (live, min_latency, max_latency)
if (pipeline latency 속성이 NONE) latency = min_latency;
else {
  if (latency < min_latency) WARNING("Configured latency is lower than detected minimum ... drop lots of data");
  if (max_latency < latency) WARNING("Impossible to configure latency ... Add queues or other buffering elements.");
}
gst_element_send_event(pipeline, gst_event_new_latency(latency));   // 모든 요소에 같은 latency 배포
```
- 파이프라인 `latency` 속성의 **기본값은 `GST_CLOCK_TIME_NONE`** (확인: gstpipeline.c:102)이라, 따로 설정하지 않으면 위 코드의 첫 분기(`latency = min_latency`)를 탄다.
- 질의 결과 합산 규칙(확인: gstbin.c:4162 주석, `bin_query_latency_fold`): 하위 요소들의 **min latency 중 최댓값**을 합산 min 으로 삼는다. `gst_bin_do_latency_func`(gstbin.c:2729)도 같은 방식으로 `min_latency` 를 배포하고, 요소가 latency 메시지를 올리면 `gst_bin_recalculate_latency` 를 다시 수행한다(gstbin.c:2852). 즉 **지연이 바뀌면 파이프라인이 다시 계산해서 재배포**한다.
- 소스들이 보고한 **최소 지연의 최댓값**이 파이프라인 latency 가 되고, 모든 싱크에 같은 값이 배포된다. 영상 소스(1프레임, 6절)와 오디오 소스(`latency-time`)처럼 지연이 다른 소스가 있어도 싱크가 `latency` 만큼 재생 시각을 뒤로 미뤄 같이 맞춘다.
- 설정한 latency 가 최소보다 작으면 **경고만 내고 진행한다**("많은 데이터를 버리게 될 것"). 강제로 막지는 않는다.

## 6-3. 싱크의 시각 계산과 대기 (확인: gstbasesink.c:2348-2500, :2737-2960)

버퍼 하나를 받았을 때 `gst_base_sink_do_sync()`:
```c
// 1) 이 버퍼의 running time (rstart, rstop) 을 segment 기준으로 계산
// 2) 프레임 간격 이동평균 갱신: avg_in_diff = UPDATE_RUNNING_AVG(avg_in_diff, rstart - prev_rstart)
// 3) QoS 로 정해진 earliest_in_time 보다 이르면 렌더 없이 드롭 (qos_dropped)
// 4) preroll (PAUSED 에서는 시계가 없어 동기하지 않음)
// 5) stime = adjust_time(rstart):  time += latency;  time ±= ts_offset;  time -= render_delay
// 6) wait_clock(stime):  time += base_time;  gst_clock_id_wait(id, &jitter)
// 7) status/jitter 로 is_too_late 판단 → late 이면 렌더 안 함
```
- 대기 시각 = `base_time + rstart + latency + ts_offset − render_delay`. 영상과 오디오 싱크가 **같은 식**으로 같은 시계에 대기하므로 A/V 가 같은 시간축에서 재생된다.
- 같은 `clock_id` 를 재사용한다(`cached_clock_id` + `gst_clock_single_shot_id_reinit`, :2440-2450). 프레임마다 대기 객체를 새로 만들지 않아 할당 비용을 줄이는 최적화.
- `jitter` 는 시계 대기가 돌려준 지연량(+이면 늦음)이고, `is_too_late` 와 QoS 계산이 이 값을 쓴다.

### QoS (확인: gstbasesink.c:3008-, gstbasesrc.c:2113-2125)
- `gst_base_sink_perform_qos()` 가 프레임마다 처리 시간 이동평균 `avg_pt`, 처리 시간/프레임 길이 비율 `avg_rate` 를 갱신하고, 값이 유효하면 **QoS 이벤트(proportion, diff, timestamp)를 상류로 보낸다.** 전송 조건(확인: gstbasesink.c:3092-3125): `avg_rate >= 0` 이면 매 프레임 보낸다. 종류는 `throttle_time > 0` 이면 `THROTTLE`(diff = throttle_time), 아니면 `diff = current_jitter` 로 두고 `diff <= 0` 이면 `OVERFLOW`, `diff > 0`(늦음) 이면 `UNDERFLOW`. 이벤트 값은 `(proportion = avg_rate, timestamp = current_rstart, diff)`. `current_jitter` 가 음수일 때는 `rstart` 아래로 내려가지 않게 보정한다.
- 상류의 `GstBaseSrc` 는 QoS 이벤트를 받으면 `proportion` 과 `earliest_time = timestamp + diff` 를 **저장만** 한다(`gst_base_src_update_qos`). `earliest_time` 은 gstbasesrc.c 에서 선언(:258)과 저장(:2122)에만 나오고 **읽는 곳이 없다.** `gstvideotestsrc.c` 와 `gstpushsrc.c` 에도 `earliest_time`/`proportion` 참조가 없다(grep 확인). 즉 이 소스들은 QoS 를 받아도 아무 동작도 하지 않는다. (`ximagesrc`·`v4l2src` 의 QoS 사용 여부는 확인하지 않았다.)
- 즉 GStreamer 는 "하류가 느리면 하류가 상류에 알리고, 상류(변환 요소/서브클래스)가 프레임 생성이나 처리를 줄이는" 구조이고, **베이스 소스 자체는 틱을 늦추거나 건너뛰지 않는다.** 이 점이 "소스에서는 드롭하지 않는다"는 정책(4절)의 근거다.

## 6-4. 오디오 소스의 타임스탬프 (확인: gstaudiobasesrc.c:751-1060)

`gst_audio_base_src_create()` 가 오디오 버퍼 하나를 만들 때:
```c
read   = gst_audio_ring_buffer_read(ringbuffer, sample, ptr, samples, &tmp_ts);   // 링버퍼에서 읽음
timestamp = scale(sample, SECOND, rate);                                           // 샘플 번호에서 계산
duration  = scale(next_sample, SECOND, rate) - timestamp;                          // 인접 차이
```
- **영상과 같은 패턴**: 샘플 번호에서 정수 스케일로 시각을 계산하고, 길이는 인접 값의 차이로 구한다. 누적 오차가 없다.
- 샘플 번호가 이어지지 않으면(`sample != next_sample`) `DISCONT` 플래그를 세우고 경고("Can't record audio fast enough ... Dropped N samples")를 낸다. 즉 **오디오는 샘플 연속성이 기준이고, 끊기면 알려 준다.**

시계에 맞추는 방식은 오디오 소스의 시계가 파이프라인 시계와 같은지에 따라 갈린다.

**(a) 파이프라인 시계 == 오디오 소스 자신의 시계 (슬레이브 아님)**
```c
timestamp = rb_timestamp (드라이버가 준 시각) 또는 gst_audio_clock_adjust(clock, timestamp);
timestamp -= base_time;  (base_time 보다 작으면 0)
```
- 오디오 HW 클럭이 곧 파이프라인 시계이므로 보정 없이 `base_time` 만 빼면 된다.

**(b) 다른 시계에 슬레이브된 경우** (`slave_method`, 기본 `SKEW`, :67)
- `SKEW`: 파이프라인 시계의 running time 을 샘플/세그먼트로 환산해 링버퍼에 **실제로 기록된 마지막 세그먼트**와 비교한다(`segment_skew = running_time_segment − last_written_segment`).
  - `segment_skew >= segtotal`(링버퍼 전체 길이만큼 뒤처짐) 이거나, 첫 샘플이거나 `last_read_segment == 0` 일 때만 **링버퍼를 `segment_diff` 만큼 앞으로 이동(`gst_audio_ring_buffer_advance`)**하고 타임스탬프를 새로 계산한다. 이때 그 구간의 오디오 데이터는 건너뛴다.
  - 그 외에는 개입하지 않는다. 즉 **작은 드리프트는 그대로 두고, 링버퍼 길이만큼 벌어졌을 때만 한 번에 재동기**한다.
  - 재동기 임계 = 링버퍼 길이(기본 `buffer-time` 200ms, `latency-time` 10ms, :62-63).
- `RESAMPLE`: 구현되어 있지 않고 `SKEW` 로 처리된다(코드 주석: "Not implemented, use skew algorithm").
- `RE_TIMESTAMP`: `timestamp = clock_time − base_time − latency(= total_samples 길이)` 로 매번 다시 찍는다. 드리프트 보정은 파이프라인의 다른 곳이 해야 한다고 주석에 명시.
- `NONE`: 샘플 번호 기반 타임스탬프를 그대로 쓴다.

참고: 오디오 소스가 시계를 제공하는 쪽(`provide_clock`, :264)이면 (a) 가 되어 영상 소스 등이 이 시계를 기준으로 맞춘다.

### 오디오 시계 구현 (확인: gstaudioclock.c)
- `GstAudioClock` 은 **시스템 시계의 서브클래스**이고, 내부 시각만 오디오 쪽 함수가 준다: `internal_time = func(clock, user_data) + time_offset`.
- 단조 보장: 결과가 `last_time` 보다 작으면 `last_time` 을 돌려준다("clock must be increasing"). 함수가 `GST_CLOCK_TIME_NONE` 을 주면(디바이스 정지 등) `last_time` 을 그대로 쓴다.
- `gst_audio_clock_reset(time)`: 시계가 되감기지 않도록 `time_offset = last_time − time` 으로 다시 맞춘다. 즉 **디바이스를 재시작해 카운터가 0 으로 돌아가도 파이프라인 시계는 연속**이다.
- `gst_audio_clock_adjust(time) = time + time_offset`: 오디오 소스가 링버퍼 시각을 시계 시각으로 변환할 때 쓴다(위 (a) 의 `gst_audio_clock_adjust`).
- `GST_CLOCK_FLAG_CAN_SET_MASTER` 가 켜져 있어 다른 시계를 마스터로 삼아 보정(슬레이빙)할 수 있다.
- 대기(`clock_id_wait`)는 시스템 시계의 구현을 그대로 상속한다(5절).

## 6-7. 라이브 캡처 소스의 타임스탬프 — `ximagesrc`, `v4l2src` (확인)

앞의 `videotestsrc`(`do_sync` 경로)는 "합성 소스"의 모델이다. 실제로 장치/화면에서 프레임을 가져오는 소스는 다르게 동작한다. 이 프로젝트의 CEF 화면 캡처와 HDMI-RX 입력에 각각 대응한다.

### (1) `ximagesrc` — 고정 fps 로 화면을 *찍는* 소스 (gstximagesrc.c:853-952)
```c
base_time        = element->base_time;
next_capture_ts  = gst_clock_get_time(clock) - base_time;           // "지금" 의 running time
next_frame_no    = scale(next_capture_ts, fps_n, SECOND * fps_d);   // 지금이 몇 번째 칸인지
if (next_frame_no == last_frame_no) {                               // 아직 같은 칸 → 다음 칸 경계까지 대기
    next_frame_no += 1;
    next_capture_ts = scale(next_frame_no, fps_d * SECOND, fps_n);  // 격자 위의 시각
    gst_clock_id_wait( base_time + next_capture_ts );
    dur = 1프레임;
} else {                                                            // 이미 다음 칸 이후 → 대기 없이 즉시 캡처
    next_frame_ts = scale(next_frame_no + 1, ...);
    dur = next_frame_ts - next_capture_ts;                          // PTS 는 "지금", 길이는 다음 경계까지
}
last_frame_no = next_frame_no;
... XGrabServer → 화면 캡처 ...
PTS = next_capture_ts;  DTS = NONE;  duration = dur;
```
핵심은 `videotestsrc` 와 반대다.
1. **프레임 번호를 누적(`n++`)하지 않고 현재 시각에서 역산한다**(`next_frame_no = floor(now × fps)`). 번호가 시계에서 나온다.
2. **늦으면 건너뛴다.** 한참 늦게 돌아오면 `next_frame_no` 가 `last_frame_no + 2` 이상이 되어 **번호가 점프**한다. 밀린 번호를 채우려고 연속 캡처하지 않는다. 화면은 "그 시점의 상태를 찍는" 것이라 과거 번호를 따라잡아도 같은 현재 화면일 뿐이기 때문이다.
3. 늦은 프레임의 PTS 는 격자 위가 아니라 **실제 캡처 시각**(`next_capture_ts`)이고, `duration` 은 다음 격자 경계까지로 줄어든다. 정시에 깨어난 프레임만 PTS 가 격자 위에 놓인다.
4. 같은 칸 안에서 또 호출되면 다음 칸까지 기다린다. 즉 **칸당 최대 1프레임**이다.

### (2) `v4l2src` — HW 가 프레임을 주는 소스 (gstv4l2src.c:1150-1290)
```c
abs_time = gst_clock_get_time(pipeline_clock);        // DQBUF 가 돌아온 직후의 시계 값
// 커널 버퍼 타임스탬프로 장치 지연을 추정
delay = now(CLOCK_MONOTONIC) - driver_timestamp;      // 캡처 후 지난 시간
timestamp = abs_time - base_time - delay;             // 캡처 "순간"의 running time
```
- **격자가 없다.** 프레임은 HW 가 만든 시각에 도착하고, 소스는 대기하지 않는다. PTS = (도착 시각의 running time) − (드라이버 타임스탬프로 추정한 지연).
- 드라이버 타임스탬프를 믿기 전에 **검증**한다: ①미래 시각이면 거부 ②직전보다 과거로 가면 거부 ③`delay > timestamp`(어느 시계와도 상관없음)이면 거부. 한 번 걸리면 `has_bad_timestamp = TRUE` 로 이후 드라이버 시각을 쓰지 않고 "지연 = 1프레임"으로 가정한다.
- 드라이버 시각이 `CLOCK_MONOTONIC` 인지 실시간 시계인지 확인해서(`timestamp > gstnow` 이거나 10초 넘게 차이 나면) 실시간 시계로 전환한다.
- 지연 보고(latency query): `min = 1프레임`, `max = 버퍼 수 × 1프레임`, `live = TRUE` (gstv4l2src.c:967-1025).

## 6-8. GStreamer 소스의 세 가지 모델과 이 프로젝트의 정책 선택

| 소스 | 모델 | 프레임 번호 | 늦으면 | PTS |
|---|---|---|---|---|
| `videotestsrc` (`do_sync`) | 번호 → 시각 | 누적(`n++`) | **연속 생성으로 따라잡음**, 번호 유지 | 격자 위 |
| `ximagesrc` | 시각 → 번호 | `floor(now×fps)` 로 역산 | **번호 점프(건너뜀)**, 따라잡지 않음 | 정시는 격자 위, 늦으면 실제 캡처 시각 |
| `v4l2src` | HW 도착 | HW 시퀀스 | 해당 없음(HW 가 박자를 정함) | 도착 시각 − 추정 지연 |

**정책 결정 시 주의 (확정 필요)**: 앞서 문서에 "GStreamer 와 똑같이"라고 정한 정책(4절의 결정 단락과 8절)은 `videotestsrc` 모델(연속 따라잡기)을 기준으로 적은 것이다. 그런데 이 프로젝트의 EncodeLoop 는 **"그 시점의 CEF 화면을 찍어 인코딩"하는 샘플러**라서 구조상 `ximagesrc` 에 더 가깝다.
- `videotestsrc` 모델: 밀린 틱마다 프레임을 만든다. 그런데 이 프로젝트에서 밀린 틱은 새 UI 그림이 없는 틱이라 **직전 그림을 재인코딩한 중복 프레임**을 만들게 된다(출력은 정확하지만 지터 버퍼를 소비할지 등 추가 결정이 필요).
- `ximagesrc` 모델: 밀린 번호를 건너뛰고 현재 상태 한 프레임만 만든다. PTS 에 구멍이 생기는 대신 **PTS 가 실제 시각과 일치**해서 오디오(실시간 기준)와 어긋나지 않는다. 현재 코드의 `next = tick_now` 리셋과 비슷해 보이지만 결정적 차이가 있다: **`ximagesrc` 는 번호를 시각에서 역산해 PTS 에 반영**하고, 현재 코드는 건너뛴 시간이 PTS(`n_`)에 반영되지 않아 시간이 사라진다.
- 두 모델 모두 공통점: **시각(PTS)과 틱 번호가 시계에서 일관되게 유도된다.** 어느 쪽을 택해도 "번호 = Write 수"인 현재 `n_` 방식은 쓰지 않는다.
- 이 선택은 정책의 핵심이라 구현 전에 사용자 확인이 필요하다. 의사결정 질문: 밀린 틱을 **(A) 중복 프레임으로 채울지(CFR 유지, `videotestsrc` 모델)**, **(B) 번호를 건너뛰고 PTS 를 시계에서 역산할지(`ximagesrc` 모델)**.

## 6-5. `videorate` — 불규칙한 입력을 CFR 로 만드는 방식 (확인: gstvideorate.c:1878-2220)

GStreamer 에서 "프레임이 늦거나 빠진 입력을 일정한 프레임레이트로 맞추는" 일은 소스가 아니라 **별도 요소(`videorate`)가 하류에서** 한다.
- 출력 시각은 **카운터 기반 격자**다: `out_frame_count` 를 출력 fps 로 스케일해 `next_output_ts` 를 만든다(gstvideorate.c:703 주석). 입력이 어떻게 도착하든 출력 PTS 는 격자 위에 놓인다.
- 입력 버퍼가 도착할 때마다 직전 버퍼(`prevbuf`)와 새 버퍼 중 **어느 쪽이 다음 출력 시각(`best_input_ts`)에 더 가까운지** 비교한다(`diff1`: prev 와의 거리, `diff2`: new 와의 거리, `new_pref` 가중치).
  - prev 가 더 가까우면 prev 를 출력하고(`flush_prev`), 같은 비교를 반복한다. 한 입력 구간에서 prev 가 여러 번 출력되면 **중복(dup)**.
  - 한 번도 출력되지 않은 채 new 로 교체되면 **드롭(drop)**.
- 통계: `in`, `out`, `dup`, `drop` 카운터를 유지한다.
- `max-duplication-time`(기본 0): 연속 프레임 간격이 이 값보다 크면 중복하지 않는 제한. 기본 0 은 "제한 없음"으로 설명되어 있다(속성 설명 기준, 코드의 분기 동작은 확인하지 않음).
- 요점: **CFR 보장은 "격자 + 가장 가까운 입력 선택, 모자라면 중복, 넘치면 드롭"** 으로 하고, 이 일은 소스 쪽 루프(`do_sync`)와 분리되어 있다.

## 6-6. 종합: GStreamer 의 시간 변환 체인

```
[소스 내부]  sample/frame 번호 n ──scale──▶ PTS_src(n)              (정수 스케일, 오차 누적 없음)
[소스 do_sync] PTS_src + ts_offset  = running time (시계 축으로 이동, ts_offset 은 첫 버퍼에서 1회 고정)
[대기]       wait( base_time + running time )  ← CLOCK_MONOTONIC 절대 시각, 늦으면 즉시 반환(EARLY)
[싱크]       wait( base_time + rstart + latency + ts_offset − render_delay )
[QoS]        싱크 → 상류: proportion/diff/timestamp 통보. 소스는 저장만, 변환 요소가 처리
[CFR 변환]   videorate: 출력 격자(카운터) 위에서 dup/drop
```
- 모든 단계에서 시각은 **번호(카운터)를 정수 스케일**해서 만든다. 시계 값을 더해 가며 누적하는 곳이 없다.
- 시계가 흔들려도(대기가 늦어도) 격자는 이동하지 않는다. 이동하는 경우는 오디오의 `SKEW` 재동기처럼 **명시적으로 임계를 넘었을 때 한 번에** 하는 경우뿐이다(6-4).
- 드롭/중복 같은 "품질 판단"은 소스가 아니라 **싱크(`max_lateness`, QoS)와 `videorate`** 가 한다.

### 이 프로젝트에 주는 시사점 (설계안 8절을 보완)
1. **`t0` = `base_time`**: 영상 틱 번호와 오디오 블록 번호를 모두 공통 `t0` 에 대한 오프셋으로 둔다(6-1). 한 번 정한 `t0` 는 이후 바꾸지 않는다.
2. **소스(EncodeLoop)는 드롭하지 않는다**(6-3): 이미 결정된 정책과 일치한다. 품질 판단은 UDP 수신 측이나 별도 변환 단계의 몫이다.
3. **CFR 을 엄격히 지켜야 한다면 `videorate` 방식**(6-5): 틱마다 "새 UI 그림이 있으면 새 것, 없으면 직전 그림을 재인코딩(중복)"이 이에 해당한다. 현재 EncodeLoop 이 이미 "직전 NV12 버퍼를 그대로 재인코딩"하는 구조라, **Encode 가 실패한 틱도 중복으로 채울지 PTS 구멍으로 둘지**를 같은 기준으로 정하면 된다(8-3 의 결정 사항).
4. **오디오는 이미 `t0_ + n×블록` 격자**(7절)이므로 GStreamer 의 (a)/`NONE` 방식에 가깝다. 영상과 같은 `t0` 만 공유하면 된다. 별도의 SKEW 재동기가 필요할 만큼 벌어지는지는 측정으로 확인해야 한다(현재 `PushLive` 는 100ms 넘게 어긋나면 재동기: `audio_mixer.cpp:72`).
5. **링버퍼 길이 = 재동기 임계**(6-4): 오디오를 한 번에 재동기해야 한다면 임계를 임의로 두지 말고 오디오 믹서가 이미 쓰는 길이 기준(예: `PushAt` 의 미래 3초 상한, `audio_mixer.cpp:63`)에 맞추는 식으로 명시적 기준을 정한다.

## 7. 이 프로젝트의 현재 구조와 문제점 (확인: 이 저장소 코드)

### 영상 (`main.cpp` EncodeLoop, `ts_muxer.cpp`)
```cpp
next += period;                       // 주기 누적
sleep_until(next);
tick_now = now();
if (tick_now - next >= period) next = tick_now;   // :556 늦으면 격자를 현재로 이동
...
if (!g_encoder.Encode([&](d,n){ mux.Write(d,n); })) continue;   // :628 실패하면 Write 안 함
```
```cpp
pkt->pts = pkt->dts = av_rescale_q(n_, {1,fps_}, time_base);    // ts_muxer.cpp:103
n_++;                                                           // Write 가 불릴 때만 증가
```
- `n_` 은 "틱 번호"가 아니라 **"실제로 Write 된 패킷 수"** 다. 따라서 (a) 늦은 틱을 리셋으로 건너뛰거나 (b) `continue`(Convert 실패/Encode 실패/`!cur.seq`/`!g_ready`)로 Write 가 빠지면 **그 시간만큼 PTS 가 실시간보다 뒤처진다.**
- `base_`(영상 시간축의 0점)는 **첫 `Write()` 시각**(`ts_muxer.cpp:109`)이라, 지터 버퍼 프라이밍(~83ms)과 인코더 첫 출력 지연이 0점에 섞여 들어간다.

### 음성 (`audio_mixer.cpp`)
- 이미 **GStreamer 와 같은 격자 방식**이다: `start = t0_ + n × 1024/48000` (`:93`), 누적하지 않고 `n` 에서 직접 계산. `t0_` 는 `AudioMixer::Start()` 시각(`:21`).
- `WriteAudio(pcm, start)` 는 `pts = (start − base_) × 48000` (`ts_muxer.cpp:123`) — 즉 **오디오의 t0_ 와 영상의 base_ 가 서로 다른 시각**이고, 그 차이를 시각 연산으로 이어 붙인다.

### 정리: 문제의 핵심
1. 영상 PTS 는 틱 수가 아니라 Write 수 → 틱이 소실되면 영상이 실시간 대비 뒤처지고, 오디오는 실시간 기준이라 **A/V 가 어긋난다.** 어긋남이 누적되는지는 측정하지 않음(미확인).
2. 영상과 오디오가 서로 다른 0점(`base_` vs `t0_`)을 쓴다.
3. 늦은 틱에서 격자가 이동한다(위상 상실).

## 8. 적용 설계안 (미적용)

GStreamer 에서 가져올 원칙 3개:
1. **하나의 기준 시각 `t0` 에서 영상·음성 PTS 를 모두 만든다.**
2. **대기 시각 = `t0 + scale(n, 1e9/fps)`.** 누적하지 않고 틱 번호 `n` 에서 직접 계산한다. 늦어도 격자를 옮기지 않는다.
3. **PTS = 틱 번호 `n` 에서 계산.** Write 성공 여부와 무관하게 `n` 은 틱마다 증가한다.

### 8-1. EncodeLoop
```cpp
const auto t0 = clk::now();             // 영상/음성 공통 기준 (아래 8-2)
uint64_t tick = 0;
while (!g_quit) {
  const auto due = t0 + ns(scale(tick, 1'000'000'000ULL, fps));   // 누적 X, 틱 번호에서 계산
  sleep_until(due);
  const int64_t late_ns = (clk::now() - due).count();
  ... 기존 처리(UI 선택/합성/Encode) ...
  // 이번 틱의 PTS 는 tick (Write 에 틱 번호를 넘긴다)
  tick++;
}
```
- 늦으면 `sleep_until` 이 즉시 반환 → **밀린 틱을 연속으로 처리해 따라잡는다**(GStreamer 와 동일, 프레임 수 유지).

> **확정 (2026-10-05, 사용자): (A) `videotestsrc`(`do_sync`) 모델.** GStreamer 의 화면 캡처 소스 `ximagesrc` 는 늦으면 번호를 건너뛰는 다른 모델(6-7, 6-8)이지만, 이 프로젝트는 (A) 연속 따라잡기 + 밀린 틱은 직전 그림을 재인코딩한 중복 프레임으로 채워 **CFR 을 유지**하는 쪽으로 정했다. `ximagesrc` 모델(B)은 채택하지 않는다.

**정책 결정 (2026-10-05, 사용자): GStreamer 와 똑같이 구현한다.**
- 격자는 절대 이동하지 않는다. 재앵커(격자 평행이동)·임계값·틱 소멸·소스 쪽 드롭을 **두지 않는다.** 늦으면 항상 연속 생성으로 따라잡는다.
- 앞서 이 문서 초안에 있던 "임계 초과 시 재앵커" 규칙은 GStreamer 소스에 없는 임의 추가라서 **채택하지 않는다.**
- 드롭이 필요한 경우의 판단은 소스(EncodeLoop)가 아니라 하류(싱크 역할)의 몫이라는 GStreamer 구조를 따른다. 이 프로젝트에서 하류에 해당하는 것은 UDP 수신 단말이므로 EncodeLoop 에서는 드롭 로직을 추가하지 않는다. (기존 UI 지터 버퍼의 오버플로 drop 은 소스 앞단의 별개 로직이라 그대로 둔다.)
- 알려진 위험(9절 2번): 큰 정지 뒤 연속 따라잡기 구간에서 UI 지터 버퍼를 매 틱 소비하면 `uiq` 가 비어 `under` 가 늘 수 있다. GStreamer 정책에는 해당 개념이 없으므로 **우선 현행 동작(틱마다 큐 1장 소비) 그대로 두고 단말에서 측정한 뒤** 필요할 때만 별도 결정한다.

### 8-2. 공통 기준 시각 (영상·음성)
- `t0` 를 한 곳(예: EncodeLoop 시작 직전)에서 잡아 `TsMuxer` 와 `AudioMixer` 에 같이 넘긴다.
- 영상: `pts = tick × 90000 / fps` (정수 스케일, 59.94 대응 시에도 오차 누적 없음).
- 음성: 블록 `n` 의 PTS = `n × 1024 × 90000/48000` (지금은 `(start − base_) × 48000` 로 시각을 거쳐 계산). `AudioMixer::t0_` 를 공통 `t0` 로 대체하면 `base_`/`base_set_`/"영상 시작 전 음성은 버림" 로직이 단순해진다.
- 시작 직후 `g_ready`/지터 버퍼 프라이밍 동안은 틱을 돌리되 인코딩하지 않으므로(현 구조), PTS 0점이 로딩 시간만큼 앞서게 된다. 영상 첫 프레임 PTS 가 0 이 아니어도 TS 에서는 문제가 아니지만, 오디오와 동일 `t0` 이면 일관된다.

### 8-3. 인코딩 건너뜀(`continue`) 처리
- GStreamer 소스는 건너뛰지 않는다. 우리도 `Encode` 실패/Convert 실패 시 **같은 틱 번호의 PTS 는 소비된 것으로 두고**(구멍) 다음 틱으로 진행하는 것이 가장 단순하다. CFR 을 엄격히 유지하려면 직전 NV12 버퍼를 재인코딩해 틱마다 패킷 1개를 보장(수신 측 요구에 따라 선택).
- 별도 결정 필요: 수신 측(UDP TS 를 받는 단말)이 PTS 구멍(VFR)을 허용하는지.

### 8-4. TsMuxer 변경
- `Write(const uint8_t*, size_t, int64_t tick)` 로 틱 번호를 받아 `pts = rescale(tick, {1,fps}, 90000)`. `n_` 제거.
- `WriteAudio` 의 `block_start`/`base_` 기반 계산을 블록 번호 기반으로 교체.
- `max_delay`/`resend_headers` 등 TS 설정은 그대로.

## 9. 위험과 확인 사항

1. **인코더 지연과 PTS 순서**: `Encode()` 는 `encode_put_frame` 직후 `encode_get_packet` 을 호출하는 동기 구조이고 B-프레임이 없다(`ts_muxer.cpp:48` `video_delay = 0`). 따라서 패킷 ↔ 틱 1:1 대응이 보장된다고 보고 있으나, `encode_get_packet` 이 실패(패킷 없음)하는 틱이 있는지는 로그로 확인 필요(현재 `continue` 로 조용히 넘어감).
2. **연속 따라잡기와 `under`/`drop` 의 상호작용**: 정책은 GStreamer 와 동일(재앵커 없음)로 확정. 큰 정지 직후 따라잡기 틱이 UI 큐를 연속 소비해 `under` 가 늘지 않는지 단말에서 측정해야 한다(`[stat]` 의 `under`/`uiq`).
3. **오디오와 `t0` 공유 시 시작 순서**: 현재 `g_audio.Start(&mux)` 가 EncodeLoop 시작 직후 호출된다(`main.cpp:535`). 공통 `t0` 를 먼저 만들고 두 쪽에 주입하는 순서로 바꿔야 한다.
4. **틱 지터 측정이 선행되어야 한다**: 현재 `[stat]` 에 틱 지연(`late_ns` 최대/횟수)이 없다. 변경 전후를 비교하려면 `tick_late_max`, `tick_late_cnt` 를 `[stat]` 에 추가. (재앵커를 두지 않기로 했으므로 `reanchor_cnt` 는 불필요.)
5. **정책 모델 선택(6-8): (A) 연속 따라잡기(`videotestsrc`)로 확정됨.** 남은 구현 세부: 밀린 틱에서 UI 지터 버퍼를 소비할지(현행대로 틱마다 1장 소비, 측정 후 판단), `Encode`/`Convert` 실패 틱도 직전 그림 중복으로 채워 CFR 을 유지할지(8-3, (A) 이므로 중복으로 채우는 쪽이 일관됨).
6. **여전히 미확인**: 배포판(Debian 12) GStreamer 패키지가 futex 옵션으로 빌드됐는지(일반 빌드는 futex 구현, `CLOCK_MIN_WAIT_TIME` 100ns), `ximagesrc`/`v4l2src` 가 QoS 이벤트를 쓰는지, `gst_clock_id_wait` 의 비동기(`gst_system_clock_async_thread`) 경로, `videorate` 의 `new_pref` 기본값과 `max-duplication-time` 분기 동작, 싱크 `rate control`(`rc_time`/`rc_next`) 세부.

### 해소된 미확인 (2026-10-05 추가 분석)
| 항목 | 결과 |
|---|---|
| `CLOCK_MIN_WAIT_TIME` | futex 100ns / pthread 500ns / GCond 1µs(Windows 1ms). 일반 Linux 빌드는 futex |
| 파이프라인 `delay` 기본값 | 0 |
| 파이프라인 `latency` 기본값 | `GST_CLOCK_TIME_NONE` → `min_latency` 사용 |
| QoS 전송 조건 | `avg_rate >= 0` 이면 매 프레임 전송, jitter 부호로 OVERFLOW/UNDERFLOW, throttle 이면 THROTTLE |
| 소스의 QoS 사용 | `GstBaseSrc` 는 저장만, `videotestsrc`/`pushsrc` 는 미사용 |
| 오디오 시계 구현 | 시스템 시계 서브클래스, `time_offset` 으로 되감기 방지, 단조 보장 |
| 라이브 캡처 소스 타임스탬프 | `ximagesrc`: 시각→번호 역산·건너뜀, `v4l2src`: 도착 시각 − 추정 지연 |
| `gstbin.c` latency 재계산 | min 중 최댓값 합산, latency 메시지 수신 시 재계산·재배포 |

## 10. 요약

| 항목 | GStreamer | 현재 cg-streamer | 적용안 |
|---|---|---|---|
| PTS 계산 | `n_frames` 정수 스케일 | `n_`(Write 수) 정수 스케일 | **틱 번호** 정수 스케일 |
| 대기 시각 | `base + ts_offset + PTS` (격자 고정) | `next += period`, 늦으면 `next = now` | `t0 + scale(tick)` (격자 고정) |
| 늦을 때 | 즉시 반환(EARLY), 연속 생성으로 따라잡음, 소스 드롭 없음 | 틱 소멸 + 격자 이동 | **GStreamer 와 동일**: 연속 따라잡기, 재앵커/드롭 없음 (결정됨) |
| 드롭 | 싱크가 `max_lateness`+QoS 로 판단, 1초 무렌더 시 강제 렌더 | UI 큐 오버플로 시 1장 drop(지터 버퍼) | 유지 |
| 시간축 | 파이프라인 시계 하나 | 영상 `base_`, 음성 `t0_` 별개 | 공통 `t0` |
| 대기 구현 | cond timedwait + 마지막 500µs `clock_nanosleep(ABSTIME)` | `sleep_until` | 유지(필요하면 2단계 대기 검토) |
