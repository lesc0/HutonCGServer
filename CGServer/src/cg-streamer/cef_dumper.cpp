#include "cef_dumper.h"

#include <cstdio>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

static void PrintErr(const char* what, int r) {
  char buf[128];
  av_strerror(r, buf, sizeof buf);
  fprintf(stderr, "[cef-dump] %s: %s\n", what, buf);
}

bool CefDumper::Open(const std::string& path, int w, int h, int fps_hint) {
  w_ = w;
  h_ = h;

  avformat_alloc_output_context2(&fmt_, nullptr, nullptr, path.c_str());
  if (!fmt_) { fprintf(stderr, "[cef-dump] alloc output context failed: %s\n", path.c_str()); return false; }

  const AVCodec* codec = avcodec_find_encoder_by_name("libx264");
  if (!codec) { fprintf(stderr, "[cef-dump] libx264 encoder not found\n"); Close(); return false; }

  st_ = avformat_new_stream(fmt_, nullptr);
  enc_ = avcodec_alloc_context3(codec);
  enc_->width = w_;
  enc_->height = h_;
  enc_->pix_fmt = AV_PIX_FMT_YUV420P;
  enc_->time_base = AVRational{1, 1000};   // ms 단위 PTS: 실제 도착 간격을 그대로 보존(VFR)
  enc_->framerate = AVRational{fps_hint, 1};
  enc_->gop_size = fps_hint > 0 ? fps_hint : 30;
  av_opt_set(enc_->priv_data, "preset", "ultrafast", 0);
  av_opt_set(enc_->priv_data, "crf", "20", 0);
  av_opt_set(enc_->priv_data, "tune", "zerolatency", 0);
  if (fmt_->oformat->flags & AVFMT_GLOBALHEADER) enc_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

  int r = avcodec_open2(enc_, codec, nullptr);
  if (r < 0) { PrintErr("avcodec_open2", r); Close(); return false; }
  avcodec_parameters_from_context(st_->codecpar, enc_);
  st_->time_base = enc_->time_base;

  r = avio_open(&fmt_->pb, path.c_str(), AVIO_FLAG_WRITE);
  if (r < 0) { PrintErr("avio_open", r); Close(); return false; }
  r = avformat_write_header(fmt_, nullptr);
  if (r < 0) { PrintErr("avformat_write_header", r); Close(); return false; }

  sws_ = sws_getContext(w_, h_, AV_PIX_FMT_BGRA, w_, h_, AV_PIX_FMT_YUV420P,
                         SWS_BILINEAR, nullptr, nullptr, nullptr);
  frame_ = av_frame_alloc();
  frame_->format = AV_PIX_FMT_YUV420P;
  frame_->width = w_;
  frame_->height = h_;
  if (av_frame_get_buffer(frame_, 32) < 0) { fprintf(stderr, "[cef-dump] frame alloc failed\n"); Close(); return false; }
  pkt_ = av_packet_alloc();

  t0_ = std::chrono::steady_clock::now();
  active_ = true;
  printf("[cef-dump] 캡처 시작: %s (%dx%d, 도착시각 그대로 VFR 기록)\n", path.c_str(), w_, h_);
  return true;
}

void CefDumper::Drain(bool flush) {
  avcodec_send_frame(enc_, flush ? nullptr : frame_);
  while (avcodec_receive_packet(enc_, pkt_) == 0) {
    av_packet_rescale_ts(pkt_, enc_->time_base, st_->time_base);
    pkt_->stream_index = st_->index;
    av_interleaved_write_frame(fmt_, pkt_);
    av_packet_unref(pkt_);
  }
}

void CefDumper::PushFrame(const void* bgra) {
  if (!active_) return;
  const uint8_t* src[1] = {(const uint8_t*)bgra};
  int stride[1] = {w_ * 4};
  sws_scale(sws_, src, stride, 0, h_, frame_->data, frame_->linesize);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0_).count();
  frame_->pts = ms;
  Drain(false);
}

void CefDumper::Close() {
  if (active_) {
    Drain(true);           // 인코더에 남은 프레임 flush
    av_write_trailer(fmt_);
    printf("[cef-dump] 캡처 종료\n");
  }
  if (sws_) sws_freeContext(sws_);
  if (frame_) av_frame_free(&frame_);
  if (pkt_) av_packet_free(&pkt_);
  if (enc_) avcodec_free_context(&enc_);
  if (fmt_) {
    if (fmt_->pb) avio_closep(&fmt_->pb);
    avformat_free_context(fmt_);
  }
  sws_ = nullptr; frame_ = nullptr; pkt_ = nullptr; enc_ = nullptr; fmt_ = nullptr; st_ = nullptr;
  active_ = false;
}
