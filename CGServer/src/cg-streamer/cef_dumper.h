#pragma once
#include <chrono>
#include <cstdint>
#include <string>

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
class CefDumper {
 public:
  ~CefDumper() { Close(); }
  bool Open(const std::string& path, int w, int h, int fps_hint);
  void PushFrame(const void* bgra);   // w*h*4 바이트 BGRA, Open 에 준 크기와 동일해야 함
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
  void Drain(bool flush);
};
