#pragma once
// mp4 등 파일 -> (libavformat 분리) -> MPP 하드웨어 디코딩 -> 최신 프레임(NV12 dmabuf) 보관
// 자체 스레드에서 원본 fps 에 맞춰 프레임을 교체하고, 끝나면 처음부터 반복 재생한다.
#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include <rockchip/rk_mpi.h>

struct VideoFrameRef {   // Acquire()~Release() 동안만 유효
  int fd = -1;           // NV12 dmabuf
  int width = 0, height = 0;
  int hor_stride = 0, ver_stride = 0;   // 픽셀 단위
  int format = 0;        // RK_FORMAT_* (0 = NV12)
};

class VideoSource {
 public:
  ~VideoSource() { Stop(); }
  bool Play(const std::string& path, bool loop = true);   // 이미 재생 중이면 교체
  void Stop();
  bool Active() const { return running_; }

  // 합성 스레드: 현재 프레임 사용 시작/끝 (그 사이에는 디코더가 프레임을 바꾸지 않음)
  bool Acquire(VideoFrameRef& out);
  void Release();

 private:
  void Run(std::string path, bool loop);
  void Publish(MppFrame f);

  std::thread th_;
  std::atomic<bool> running_{false}, stop_{false};
  std::mutex mu_;
  MppFrame cur_ = nullptr;
  MppBufferGroup grp_ = nullptr;   // 디코더 출력 버퍼 (DRM, dma32 — RGA 가 fd 로 읽음)
};
