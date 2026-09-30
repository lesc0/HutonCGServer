#include "ts_muxer.h"

#include <cstdio>
#include <cstring>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
}

static void PrintErr(const char* what, int r) {
  char buf[128];
  av_strerror(r, buf, sizeof buf);
  fprintf(stderr, "[ts] %s: %s\n", what, buf);
}

// Annex-B 에서 첫 슬라이스 NAL 이 IDR(5)인지 검사
static bool IsIdr(const uint8_t* p, size_t n) {
  for (size_t i = 0; i + 3 < n; i++) {
    if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
      int t = p[i + 3] & 0x1f;
      if (t == 5) return true;
      if (t == 1) return false;
      i += 2;
    }
  }
  return false;
}

bool TsMuxer::Open(const std::string& url, int w, int h, int fps) {
  fps_ = fps;
  avformat_network_init();

  int r = avformat_alloc_output_context2(&fc_, nullptr, "mpegts", url.c_str());
  if (r < 0 || !fc_) { PrintErr("alloc output", r); return false; }

  st_ = avformat_new_stream(fc_, nullptr);
  if (!st_) { fprintf(stderr, "[ts] new_stream failed\n"); return false; }

  AVCodecParameters* cp = st_->codecpar;
  cp->codec_type = AVMEDIA_TYPE_VIDEO;
  cp->codec_id = AV_CODEC_ID_H264;
  cp->width = w;
  cp->height = h;
  cp->video_delay = 0;                 // B-프레임 없음(MPP CBR)
  st_->time_base = AVRational{1, 90000};
  st_->avg_frame_rate = AVRational{fps, 1};

  if (!(fc_->oformat->flags & AVFMT_NOFILE)) {
    r = avio_open2(&fc_->pb, url.c_str(), AVIO_FLAG_WRITE, nullptr, nullptr);
    if (r < 0) { PrintErr("avio_open", r); return false; }
  }

  fc_->max_delay = AV_TIME_BASE / 10;              // PCR 오프셋(저지연): 0.1s
  fc_->flags |= AVFMT_FLAG_FLUSH_PACKETS;          // 패킷마다 즉시 전송

  AVDictionary* opts = nullptr;
  av_dict_set(&opts, "mpegts_flags", "resend_headers", 0);  // PAT/PMT 주기 재전송
  av_dict_set(&opts, "pat_period", "0.1", 0);
  r = avformat_write_header(fc_, &opts);
  av_dict_free(&opts);
  if (r < 0) { PrintErr("write_header", r); return false; }
  header_ = true;
  printf("[ts] open: %s\n", url.c_str());
  return true;
}

bool TsMuxer::Write(const uint8_t* d, size_t len) {
  if (!fc_ || !header_ || len == 0) return false;

  AVPacket* pkt = av_packet_alloc();
  if (!pkt || av_new_packet(pkt, (int)len) < 0) { av_packet_free(&pkt); return false; }
  memcpy(pkt->data, d, len);

  pkt->stream_index = st_->index;
  pkt->pts = pkt->dts = av_rescale_q(n_, AVRational{1, fps_}, st_->time_base);
  pkt->duration = av_rescale_q(1, AVRational{1, fps_}, st_->time_base);
  if (IsIdr(d, len)) pkt->flags |= AV_PKT_FLAG_KEY;
  n_++;

  int r = av_write_frame(fc_, pkt);
  av_packet_free(&pkt);
  if (r < 0) {
    if (!warned_) { PrintErr("write_frame", r); warned_ = true; }
    return false;
  }
  return true;
}

void TsMuxer::Close() {
  if (!fc_) return;
  if (header_) av_write_trailer(fc_);
  if (fc_->pb && !(fc_->oformat->flags & AVFMT_NOFILE)) avio_closep(&fc_->pb);
  avformat_free_context(fc_);
  fc_ = nullptr;
  st_ = nullptr;
  header_ = false;
}
