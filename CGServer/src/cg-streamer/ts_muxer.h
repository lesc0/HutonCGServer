#pragma once
#include <cstddef>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

struct AVFormatContext;
struct AVStream;
struct AVCodecContext;

// MPP가 만든 Annex-B H.264 패킷 -> MPEG-TS (libavformat) -> UDP 또는 파일
class TsMuxer {
 public:
  ~TsMuxer() { Close(); }
  // url 예: "udp://239.1.1.1:1234?pkt_size=1316", "udp://192.168.0.10:5000?pkt_size=1316", "out.ts"
  bool Open(const std::string& url, int width, int height, int fps);
  // 패킷 1개 = 프레임 1개. src_ts_ns: 그 프레임의 입력 timestamp(CLOCK_MONOTONIC ns, 라이브는 V4L2 timestamp).
  // PTS 는 프레임 번호가 아니라 이 timestamp 에서 만든다(base_ 기준 상대 시간). 0 이면 지금 시각. 단, DTS 단조 증가를 위해 직전 PTS 이하면 직전+1.
  bool Write(const uint8_t* annexb, size_t len, int64_t src_ts_ns = 0);
  // AAC-LC 48kHz 스테레오 음성. PCM 은 float 인터리브 kAudioFrame(1024) 샘플/채널 단위.
  // block_start: 이 블록의 첫 샘플 시각(steady_clock). 영상 첫 패킷 시각을 0 으로 pts 를 계산한다.
  static constexpr int kAudioFrame = 1024;
  static constexpr int kAudioRate = 48000;
  bool WriteAudio(const float* pcm, std::chrono::steady_clock::time_point block_start);
  void Close();

 private:
  AVFormatContext* fc_ = nullptr;
  AVStream* st_ = nullptr;
  int fps_ = 60;
  int64_t last_vpts_ = INT64_MIN;                   // 직전 영상 PTS (90kHz), 단조 증가 보장용
  bool header_ = false;
  bool warned_ = false;

  AVStream* ast_ = nullptr;
  AVCodecContext* aenc_ = nullptr;
  std::mutex mu_;                                   // 영상/음성 스레드가 같은 muxer 에 쓴다
  std::chrono::steady_clock::time_point base_;      // 영상 첫 패킷 시각
  bool base_set_ = false;
  int64_t last_apts_ = INT64_MIN;                   // 1/48000 단위
};
