#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct AVFormatContext;
struct AVStream;
struct AVCodecContext;
struct SwsContext;
struct AVFrame;
struct AVPacket;

// 진단용: OnPaint 로 들어오는 BGRA 원본을 RGA/MPP/UDP 전혀 거치지 않고
// 소프트웨어 H.264(libx264)로 그대로 mp4 에 기록한다.
// 끊김이 CEF 가 그림을 내놓는 단계부터인지, 그 이후(합성/인코딩) 단계인지 구분하기 위한 용도.
// PTS 는 60/30fps 로 맞추지 않고 실제 OnPaint 도착 시각(ms) 그대로 써서(VFR),
// CEF 자체가 프레임을 고르게 주는지 아닌지가 영상에 그대로 드러나게 한다.
//
// 중요: PushFrame() 은 OnPaint(CEF UI 스레드) 에서 직접 불리므로 가벼운 복사만 하고,
// 느린 sws_scale/libx264 인코딩은 전부 별도 워커 스레드에서 한다. 그렇지 않으면
// 인코딩 자체가 OnPaint 를 블로킹해서 "CEF가 느리다"는 착시를 만들 수 있다.
class CefDumper {
 public:
  ~CefDumper() { Close(); }
  bool Open(const std::string& path, int w, int h, int fps_hint);
  void PushFrame(const void* bgra);   // w*h*4 바이트 BGRA, Open 에 준 크기와 동일해야 함 (가벼운 복사만, 즉시 반환)
  void Close();
  bool Active() const { return active_; }

 private:
  AVFormatContext* fmt_ = nullptr;
  AVCodecContext* enc_ = nullptr;
  AVStream* st_ = nullptr;
  SwsContext* sws_ = nullptr;
  AVFrame* frame_ = nullptr;
  AVPacket* pkt_ = nullptr;
  int w_ = 0, h_ = 0;
  std::chrono::steady_clock::time_point t0_;
  bool active_ = false;

  struct QueuedFrame { std::vector<uint8_t> bgra; int64_t ms; };
  std::deque<QueuedFrame> queue_;
  std::mutex qmu_;
  std::condition_variable qcv_;
  std::thread worker_;
  std::atomic<bool> stop_{false};
  int queue_dropped_ = 0;
  static constexpr size_t kQueueMax = 32;   // 넉넉히: 이 큐가 넘치면 그건 진짜로 인코딩이 못 따라가는 것

  void WorkerLoop();
  void EncodeOne(const QueuedFrame& f);
  void Drain(bool flush);
};
