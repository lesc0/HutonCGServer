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
