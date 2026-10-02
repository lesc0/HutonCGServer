#include "hdmirx_source.h"

#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <vector>

#include <rga/rga.h>

#include "audio_mixer.h"
#include "rt.h"

static int Xioctl(int fd, unsigned long req, void* arg) {
  int r;
  do r = ioctl(fd, req, arg);
  while (r < 0 && errno == EINTR);
  return r;
}

bool HdmiRxSource::Start(const std::string& dev) {
  Stop();
  stop_ = false;
  running_ = true;
  th_ = std::thread(&HdmiRxSource::Run, this, dev);
  if (audio_) ath_ = std::thread(&HdmiRxSource::AudioRun, this);
  return true;
}

// ---- 음성: libasound.so.2 를 dlopen (개발 헤더/링크 의존 없이 캡처만 사용) ----
// HDMI 수신 칩의 I2S 캡처 카드("rockchiphdmiin")를 plughw 로 열어 48kHz S16 스테레오로 받는다.
void HdmiRxSource::AudioRun() {
  pthread_setname_np(pthread_self(), "cg-hdmi-aud");
  SetRealtime("cg-hdmi-aud", 48);
  struct Api {
    int (*open)(void**, const char*, int, int);
    int (*set_params)(void*, int, int, unsigned, unsigned, int, unsigned);
    long (*readi)(void*, void*, unsigned long);
    int (*recover)(void*, int, int);
    int (*start)(void*);
    int (*close)(void*);
  } a{};
  void* lib = dlopen("libasound.so.2", RTLD_NOW);
  if (lib) {
    a.open = (decltype(a.open))dlsym(lib, "snd_pcm_open");
    a.set_params = (decltype(a.set_params))dlsym(lib, "snd_pcm_set_params");
    a.readi = (decltype(a.readi))dlsym(lib, "snd_pcm_readi");
    a.recover = (decltype(a.recover))dlsym(lib, "snd_pcm_recover");
    a.start = (decltype(a.start))dlsym(lib, "snd_pcm_start");
    a.close = (decltype(a.close))dlsym(lib, "snd_pcm_close");
  }
  if (!lib || !a.open || !a.set_params || !a.readi || !a.recover || !a.start || !a.close) {
    fprintf(stderr, "[hdmirx] libasound.so.2 를 쓸 수 없음 - HDMI 음성 없음\n");
    if (lib) dlclose(lib);
    return;
  }
  // snd_pcm.h 상수: STREAM_CAPTURE=1, FORMAT_S16_LE=2, ACCESS_RW_INTERLEAVED=3
  const int kCapture = 1, kS16Le = 2, kRwInterleaved = 3;
  const char* kDev = "plughw:CARD=rockchiphdmiin,DEV=0";
  constexpr int kChunk = 1024;
  std::vector<int16_t> raw(kChunk * 2);
  std::vector<float> pcm(kChunk * 2);
  void* pcm_h = nullptr;
  bool warned = false;
  while (!stop_) {
    if (!pcm_h) {   // 신호가 없거나 장치가 아직 준비 안 되면 재시도
      if (a.open(&pcm_h, kDev, kCapture, 0) < 0 ||
          a.set_params(pcm_h, kS16Le, kRwInterleaved, 2, AudioMixer::kRate, 1, 100000) < 0) {
        if (pcm_h) { a.close(pcm_h); pcm_h = nullptr; }
        if (!warned) { fprintf(stderr, "[hdmirx] 음성 캡처 열기 실패 (%s) - 재시도\n", kDev); warned = true; }
        for (int i = 0; i < 10 && !stop_; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        continue;
      }
      // 이 보드(rk_hdmirx)는 readi 의 자동 시작이 EIO 로 실패해서 명시적으로 start 해야 한다.
      if (a.start(pcm_h) < 0) {
        a.close(pcm_h);
        pcm_h = nullptr;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        continue;
      }
      warned = false;
      printf("[hdmirx] 음성 캡처 시작: %s\n", kDev);
    }
    const long n = a.readi(pcm_h, raw.data(), kChunk);
    if (n < 0) {   // overrun/신호 끊김: 복구 후 다시 start, 안 되면 닫고 다시 연다
      if (a.recover(pcm_h, (int)n, 1) < 0 || a.start(pcm_h) < 0) {
        a.close(pcm_h);
        pcm_h = nullptr;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
      }
      continue;
    }
    for (long i = 0; i < n * 2; i++) pcm[i] = raw[i] / 32768.f;
    audio_->PushLive(pcm.data(), (int)n);
  }
  if (pcm_h) a.close(pcm_h);
  dlclose(lib);
}

void HdmiRxSource::Stop() {
  stop_ = true;
  if (th_.joinable()) th_.join();
  if (ath_.joinable()) ath_.join();
  if (audio_) audio_->Clear();
  running_ = false;
  signal_ = false;
}

bool HdmiRxSource::Acquire(VideoFrameRef& out) {
  mu_.lock();
  if (cur_ < 0) { mu_.unlock(); return false; }
  out = cur_ref_;
  return true;   // Release() 에서 unlock
}

void HdmiRxSource::Release() { mu_.unlock(); }

void HdmiRxSource::Run(std::string dev) {
  pthread_setname_np(pthread_self(), "cg-hdmirx");
  SetRealtime("cg-hdmirx", 48);
  printf("[hdmirx] start %s\n", dev.c_str());
  while (!stop_) {
    if (!Session(dev)) std::this_thread::sleep_for(std::chrono::milliseconds(500));   // 신호 대기 후 재시도
  }
  printf("[hdmirx] stop\n");
}

bool HdmiRxSource::Session(const std::string& dev) {
  static bool logged_nosig = false;
  int fd = open(dev.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) { perror(dev.c_str()); return false; }

  v4l2_event_subscription sub{};
  sub.type = V4L2_EVENT_SOURCE_CHANGE;
  Xioctl(fd, VIDIOC_SUBSCRIBE_EVENT, &sub);

  v4l2_dv_timings t{};
  if (Xioctl(fd, VIDIOC_QUERY_DV_TIMINGS, &t) < 0 || t.bt.width == 0) {   // 신호 없음
    if (!logged_nosig) { printf("[hdmirx] no signal (%s)\n", strerror(errno)); logged_nosig = true; }
    signal_ = false;
    close(fd);
    return false;
  }
  logged_nosig = false;
  Xioctl(fd, VIDIOC_S_DV_TIMINGS, &t);

  v4l2_format f{};
  f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  if (Xioctl(fd, VIDIOC_G_FMT, &f) < 0) { perror("[hdmirx] G_FMT"); close(fd); return false; }
  const int w = (int)f.fmt.pix_mp.width, h = (int)f.fmt.pix_mp.height;
  const int bpl = (int)f.fmt.pix_mp.plane_fmt[0].bytesperline;
  const uint32_t pf = f.fmt.pix_mp.pixelformat;
  int fmt, stride;
  switch (pf) {   // RGA 포맷 이름은 메모리 바이트 순서 기준 (V4L2 BGR3 = B,G,R)
    case V4L2_PIX_FMT_BGR24: fmt = RK_FORMAT_BGR_888; stride = bpl / 3; break;
    case V4L2_PIX_FMT_NV12: fmt = RK_FORMAT_YCbCr_420_SP; stride = bpl; break;
    case V4L2_PIX_FMT_NV16: fmt = RK_FORMAT_YCbCr_422_SP; stride = bpl; break;
    case V4L2_PIX_FMT_NV24: fmt = RK_FORMAT_YCbCr_444_SP; stride = bpl; break;
    default:
      fprintf(stderr, "[hdmirx] 지원하지 않는 포맷 %.4s\n", (const char*)&pf);
      close(fd);
      std::this_thread::sleep_for(std::chrono::seconds(1));
      return false;
  }
  const double tw = V4L2_DV_BT_FRAME_WIDTH(&t.bt), th = V4L2_DV_BT_FRAME_HEIGHT(&t.bt);
  const double fps = (tw > 0 && th > 0) ? t.bt.pixelclock / (tw * th) : 0;

  constexpr int kBufs = 4;
  v4l2_requestbuffers req{};
  req.count = kBufs;
  req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  req.memory = V4L2_MEMORY_MMAP;
  if (Xioctl(fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) { perror("[hdmirx] REQBUFS"); close(fd); return false; }
  const int nbuf = (int)req.count;
  int fds[16];
  for (int i = 0; i < nbuf; i++) fds[i] = -1;

  auto qbuf = [&](int i) {
    v4l2_buffer b{};
    v4l2_plane pl[VIDEO_MAX_PLANES]{};
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    b.memory = V4L2_MEMORY_MMAP;
    b.index = i;
    b.m.planes = pl;
    b.length = f.fmt.pix_mp.num_planes;
    return Xioctl(fd, VIDIOC_QBUF, &b);
  };
  bool ok = true;
  for (int i = 0; i < nbuf && ok; i++) {
    v4l2_exportbuffer eb{};
    eb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    eb.index = i;
    eb.plane = 0;
    eb.flags = O_RDONLY | O_CLOEXEC;
    if (Xioctl(fd, VIDIOC_EXPBUF, &eb) < 0) { perror("[hdmirx] EXPBUF"); ok = false; break; }
    fds[i] = eb.fd;
    if (qbuf(i) < 0) { perror("[hdmirx] QBUF"); ok = false; }
  }
  v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  if (ok && Xioctl(fd, VIDIOC_STREAMON, &type) < 0) { perror("[hdmirx] STREAMON"); ok = false; }

  if (ok) {
    signal_ = true;
    printf("[hdmirx] signal %dx%d %.2ffps %.4s stride=%d\n", w, h, fps, (const char*)&pf, stride);
    int idle = 0;
    while (!stop_) {
      pollfd p{fd, POLLIN | POLLPRI, 0};
      if (poll(&p, 1, 200) <= 0) {
        if (++idle > 10) { printf("[hdmirx] 2초간 프레임 없음 - 재연결\n"); break; }
        continue;
      }
      idle = 0;
      if (p.revents & POLLPRI) {   // 해상도/신호 변경
        v4l2_event ev{};
        bool changed = false;
        while (Xioctl(fd, VIDIOC_DQEVENT, &ev) == 0)
          if (ev.type == V4L2_EVENT_SOURCE_CHANGE) changed = true;
        if (changed) { printf("[hdmirx] source change - 재연결\n"); break; }
      }
      if (p.revents & POLLIN) {
        v4l2_buffer b{};
        v4l2_plane pl[VIDEO_MAX_PLANES]{};
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        b.memory = V4L2_MEMORY_MMAP;
        b.m.planes = pl;
        b.length = VIDEO_MAX_PLANES;
        if (Xioctl(fd, VIDIOC_DQBUF, &b) < 0) continue;
        int old;
        {
          std::lock_guard<std::mutex> lk(mu_);   // 합성 중이면 끝날 때까지 대기
          old = cur_;
          cur_ = (int)b.index;
          cur_ref_.fd = fds[b.index];
          cur_ref_.width = w;
          cur_ref_.height = h;
          cur_ref_.hor_stride = stride;
          cur_ref_.ver_stride = h;
          cur_ref_.format = fmt;
        }
        if (old >= 0) qbuf(old);   // 이전 프레임은 드라이버에 반환
      }
    }
  }

  {
    std::lock_guard<std::mutex> lk(mu_);
    cur_ = -1;
  }
  signal_ = false;
  Xioctl(fd, VIDIOC_STREAMOFF, &type);
  for (int i = 0; i < nbuf; i++)
    if (fds[i] >= 0) close(fds[i]);
  req.count = 0;
  Xioctl(fd, VIDIOC_REQBUFS, &req);
  close(fd);
  return ok;
}
