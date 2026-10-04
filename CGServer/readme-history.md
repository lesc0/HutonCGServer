# 작업 히스토리 (Debian 12)

Debian 11 시절 기록은 `readme-history-d11.md` 참고.

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

#### cg-editor 빌드와 실행
- `src/cg-editor/build.sh`(`npm ci` + Next.js 빌드)가 Node 22 로 성공 → `.next/` 생성.
- `bin/start.sh` 로 cg-editor(prod, 포트 8080) 기동, `http://localhost:8080/` 응답 200. cg-streamer 는 계속 실행 중. 키오스크는 생략(`CG_SKIP_KIOSK=1`).
