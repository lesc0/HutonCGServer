#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

struct AVFormatContext;
struct AVStream;

// MPP가 만든 Annex-B H.264 패킷 -> MPEG-TS (libavformat) -> UDP 또는 파일
class TsMuxer {
 public:
  ~TsMuxer() { Close(); }
  // url 예: "udp://239.1.1.1:1234?pkt_size=1316", "udp://192.168.0.10:5000?pkt_size=1316", "out.ts"
  bool Open(const std::string& url, int width, int height, int fps);
  bool Write(const uint8_t* annexb, size_t len);   // 패킷 1개 = 프레임 1개
  void Close();

 private:
  AVFormatContext* fc_ = nullptr;
  AVStream* st_ = nullptr;
  int fps_ = 60;
  int64_t n_ = 0;
  bool header_ = false;
  bool warned_ = false;
};
