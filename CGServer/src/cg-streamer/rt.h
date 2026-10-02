#pragma once
// 실시간 스케줄링: 호출한 스레드를 SCHED_FIFO 로 올린다(표시/인코딩/음성 스레드가 다른 작업에 밀려 프레임 간격이 흔들리는 것을 줄임).
// 권한이 없으면(RLIMIT_RTPRIO=0) 한 번만 안내하고 일반 스케줄링으로 계속 동작한다.
// 권한 부여: /etc/security/limits.d/99-cg-rt.conf 에 "pi - rtprio 90" 한 줄(다시 로그인해야 적용). bin/start.sh 가 현재 한도를 안내한다.
#include <pthread.h>
#include <sched.h>

#include <atomic>
#include <cstdio>

inline void SetRealtime(const char* what, int prio) {
  static std::atomic<bool> warned{false};
  sched_param sp{};
  sp.sched_priority = prio;
  const int r = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
  if (r == 0) {
    printf("[rt] %s: SCHED_FIFO %d\n", what, prio);
  } else if (!warned.exchange(true)) {
    fprintf(stderr, "[rt] SCHED_FIFO 설정 실패(%s): 권한 없음 - 일반 스케줄링으로 동작 (limits.d 에 rtprio 허용 필요)\n", what);
  }
}
