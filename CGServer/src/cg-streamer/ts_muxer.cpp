#include "ts_muxer.h"

#include <cstdio>
#include <cmath>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
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

bool TsMuxer::Open(const std::string& url, int w, int h, int fps_num, int fps_den) {
  fps_num_ = fps_num;
  fps_den_ = fps_den;
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
  st_->avg_frame_rate = AVRational{fps_num, fps_den};

  // 음성: AAC-LC (libavcodec 내장 aac 인코더). 인코더를 먼저 열어 extradata(ADTS 용)를 스트림에 복사한다.
  const AVCodec* ac = avcodec_find_encoder(AV_CODEC_ID_AAC);
  if (ac) {
    aenc_ = avcodec_alloc_context3(ac);
    aenc_->sample_rate = kAudioRate;
    aenc_->channel_layout = AV_CH_LAYOUT_STEREO;
    aenc_->channels = 2;
    aenc_->sample_fmt = AV_SAMPLE_FMT_FLTP;
    aenc_->bit_rate = 128000;
    aenc_->time_base = AVRational{1, kAudioRate};
    aenc_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;   // mpegts 는 extradata(AudioSpecificConfig)로 ADTS 헤더를 만든다
    if (avcodec_open2(aenc_, ac, nullptr) >= 0 && (ast_ = avformat_new_stream(fc_, nullptr)) &&
        avcodec_parameters_from_context(ast_->codecpar, aenc_) >= 0) {
      ast_->time_base = AVRational{1, 90000};
    } else {
      fprintf(stderr, "[ts] AAC 인코더 초기화 실패 - 음성 없이 송출\n");
      avcodec_free_context(&aenc_);
      aenc_ = nullptr;
      ast_ = nullptr;
    }
  } else {
    fprintf(stderr, "[ts] AAC 인코더 없음 - 음성 없이 송출\n");
  }

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

bool TsMuxer::Write(const uint8_t* d, size_t len, int64_t src_ts_ns) {
  if (!fc_ || !header_ || len == 0) return false;

  AVPacket* pkt = av_packet_alloc();
  if (!pkt || av_new_packet(pkt, (int)len) < 0) { av_packet_free(&pkt); return false; }
  memcpy(pkt->data, d, len);

  pkt->stream_index = st_->index;
  pkt->duration = av_rescale_q(1, AVRational{fps_den_, fps_num_}, st_->time_base);
  if (IsIdr(d, len)) pkt->flags |= AV_PKT_FLAG_KEY;

  if (src_ts_ns <= 0)   // 입력 timestamp 가 없으면 지금 시각
    src_ts_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  // PTS = timestamp(ns)를 90kHz 로 환산만 한다 (기준점 빼기·보정 없음)
  pkt->pts = pkt->dts = av_rescale_q(src_ts_ns, AVRational{1, 1000000000}, st_->time_base);
  std::lock_guard<std::mutex> lk(mu_);
  int r = av_write_frame(fc_, pkt);
  av_packet_free(&pkt);
  if (r < 0) {
    if (!warned_) { PrintErr("write_frame", r); warned_ = true; }
    return false;
  }
  return true;
}

bool TsMuxer::WriteAudio(const float* pcm, std::chrono::steady_clock::time_point block_start) {
  return WriteAudioNs(pcm, std::chrono::duration_cast<std::chrono::nanoseconds>(block_start.time_since_epoch()).count());
}

bool TsMuxer::WriteAudioNs(const float* pcm, int64_t pts_ns) {
  std::lock_guard<std::mutex> lk(mu_);
  if (!fc_ || !header_ || !aenc_) return false;

  // PTS = timestamp(ns)를 환산만 한다 (영상과 같은 CLOCK_MONOTONIC, 기준점 빼기·보정 없음). 인코더 time_base(1/48000)로 넘기면 mux 에서 90kHz 로 바뀜
  const int64_t pts = av_rescale_q(pts_ns, AVRational{1, 1000000000}, aenc_->time_base);

  AVFrame* f = av_frame_alloc();
  f->nb_samples = kAudioFrame;
  f->format = AV_SAMPLE_FMT_FLTP;
  f->channel_layout = AV_CH_LAYOUT_STEREO;
  f->channels = 2;
  f->sample_rate = kAudioRate;
  if (av_frame_get_buffer(f, 0) < 0) { av_frame_free(&f); return false; }
  float* l = (float*)f->data[0];
  float* r = (float*)f->data[1];
  for (int i = 0; i < kAudioFrame; i++) { l[i] = pcm[2 * i]; r[i] = pcm[2 * i + 1]; }
  f->pts = pts;

  bool ok = avcodec_send_frame(aenc_, f) >= 0;
  av_frame_free(&f);
  AVPacket* pkt = av_packet_alloc();
  while (ok && avcodec_receive_packet(aenc_, pkt) == 0) {
    pkt->stream_index = ast_->index;
    av_packet_rescale_ts(pkt, aenc_->time_base, ast_->time_base);
    if (av_write_frame(fc_, pkt) < 0) ok = false;
    av_packet_unref(pkt);
  }
  av_packet_free(&pkt);
  return ok;
}

void TsMuxer::Close() {
  if (!fc_) return;
  std::lock_guard<std::mutex> lk(mu_);
  if (header_) av_write_trailer(fc_);
  if (fc_->pb && !(fc_->oformat->flags & AVFMT_NOFILE)) avio_closep(&fc_->pb);
  avformat_free_context(fc_);
  if (aenc_) avcodec_free_context(&aenc_);
  fc_ = nullptr;
  st_ = nullptr;
  ast_ = nullptr;
  header_ = false;
}
