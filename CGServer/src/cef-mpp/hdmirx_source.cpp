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

#include <rga/rga.h>

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
  return true;
}

void HdmiRxSource::Stop() {
  stop_ = true;
  if (th_.joinable()) th_.join();
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
