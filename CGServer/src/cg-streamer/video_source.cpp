#include "video_source.h"

#include <pthread.h>

#include <chrono>
#include <cstdio>
#include <functional>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
}

#include "audio_mixer.h"

using clk = std::chrono::steady_clock;

bool VideoSource::Play(const std::string& path, bool loop) {
  Stop();
  stop_ = false;
  running_ = true;
  th_ = std::thread(&VideoSource::Run, this, path, loop);
  return true;
}

void VideoSource::Stop() {
  stop_ = true;
  if (th_.joinable()) th_.join();
  running_ = false;
  if (audio_) audio_->Clear();   // 아직 재생 안 된 음성 폐기
  std::lock_guard<std::mutex> lk(mu_);
  if (cur_) mpp_frame_deinit(&cur_);
  if (grp_) { mpp_buffer_group_put(grp_); grp_ = nullptr; }   // 프레임을 모두 놓은 뒤 그룹 해제
}

bool VideoSource::Acquire(VideoFrameRef& out) {
  mu_.lock();
  if (!cur_) { mu_.unlock(); return false; }
  out.fd = mpp_buffer_get_fd(mpp_frame_get_buffer(cur_));
  out.width = (int)mpp_frame_get_width(cur_);
  out.height = (int)mpp_frame_get_height(cur_);
  out.hor_stride = (int)mpp_frame_get_hor_stride(cur_);
  out.ver_stride = (int)mpp_frame_get_ver_stride(cur_);
  return true;   // Release() 에서 unlock
}

void VideoSource::Release() { mu_.unlock(); }

void VideoSource::Publish(MppFrame f) {
  MppFrame old;
  {
    std::lock_guard<std::mutex> lk(mu_);   // 합성 중이면 끝날 때까지 대기
    old = cur_;
    cur_ = f;
  }
  if (old) mpp_frame_deinit(&old);
}

void VideoSource::Run(std::string path, bool loop) {
  pthread_setname_np(pthread_self(), "cg-video");
  AVFormatContext* fc = nullptr;
  AVBSFContext* bsf = nullptr;
  AVPacket* pkt = nullptr;
  MppCtx ctx = nullptr;
  MppApi* mpi = nullptr;

  auto fail = [&](const char* what) { fprintf(stderr, "[video] %s: %s\n", what, path.c_str()); };

  if (avformat_open_input(&fc, path.c_str(), nullptr, nullptr) < 0) { fail("open 실패"); running_ = false; return; }
  avformat_find_stream_info(fc, nullptr);
  const int si = av_find_best_stream(fc, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (si < 0) { fail("비디오 스트림 없음"); avformat_close_input(&fc); running_ = false; return; }
  AVStream* st = fc->streams[si];

  MppCodingType coding;
  const char* bsf_name = nullptr;   // mp4(AVCC) -> Annex-B (MPP 입력 형식)
  switch (st->codecpar->codec_id) {
    case AV_CODEC_ID_H264: coding = MPP_VIDEO_CodingAVC; bsf_name = "h264_mp4toannexb"; break;
    case AV_CODEC_ID_HEVC: coding = MPP_VIDEO_CodingHEVC; bsf_name = "hevc_mp4toannexb"; break;
    case AV_CODEC_ID_VP9: coding = MPP_VIDEO_CodingVP9; break;
    case AV_CODEC_ID_VP8: coding = MPP_VIDEO_CodingVP8; break;
    default: fail("지원하지 않는 코덱"); avformat_close_input(&fc); running_ = false; return;
  }
  if (bsf_name) {
    av_bsf_alloc(av_bsf_get_by_name(bsf_name), &bsf);
    avcodec_parameters_copy(bsf->par_in, st->codecpar);
    bsf->time_base_in = st->time_base;
    av_bsf_init(bsf);
  }
  // 음성 트랙: 디코드 -> 48kHz 스테레오 float -> 믹서 (영상 시각 t0 기준으로 배치)
  AVCodecContext* adec = nullptr;
  SwrContext* swr = nullptr;
  AVStream* ast = nullptr;
  int asi = -1;
  if (audio_ && (asi = av_find_best_stream(fc, AVMEDIA_TYPE_AUDIO, -1, si, nullptr, 0)) >= 0) {
    ast = fc->streams[asi];
    const AVCodec* ac = avcodec_find_decoder(ast->codecpar->codec_id);
    adec = ac ? avcodec_alloc_context3(ac) : nullptr;
    bool aok = adec && avcodec_parameters_to_context(adec, ast->codecpar) >= 0 && avcodec_open2(adec, ac, nullptr) >= 0;
    if (aok) {
      const uint64_t in_l = adec->channel_layout ? adec->channel_layout : av_get_default_channel_layout(adec->channels);
      aok = (swr = swr_alloc_set_opts(nullptr, AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_FLT, AudioMixer::kRate, in_l,
                                      adec->sample_fmt, adec->sample_rate, 0, nullptr)) &&
            swr_init(swr) >= 0;
    }
    if (!aok) {
      fail("음성 디코더 초기화 실패(음성 없이 재생)");
      swr_free(&swr);
      avcodec_free_context(&adec);
      asi = -1;
    }
  }

  double fps = av_q2d(st->avg_frame_rate);
  if (!(fps > 0 && fps < 240)) fps = av_q2d(st->r_frame_rate);
  if (!(fps > 0 && fps < 240)) fps = 30;

  if (mpp_create(&ctx, &mpi) != MPP_OK || mpp_init(ctx, MPP_CTX_DEC, coding) != MPP_OK) {
    fail("MPP 디코더 초기화 실패");
    if (bsf) av_bsf_free(&bsf);
    avformat_close_input(&fc);
    running_ = false;
    return;
  }
  printf("[video] play %s (%s, %.3ffps)\n", path.c_str(), avcodec_get_name(st->codecpar->codec_id), fps);

  auto t0 = clk::now();
  uint64_t shown = 0;
  uint64_t vpkts = 0;   // MPP 에 넣은 영상 패킷 수 (= 이 패킷의 표시 순번)

  // 음성 배치: 반복 재생 때마다 "그 회차 첫 영상 프레임의 표시 시각"에서 이어 붙여 영상과 어긋남이 쌓이지 않게 한다.
  // 첫 프레임이 표시되기 전(t0 미확정)에 읽은 음성은 모아 두었다가 t0 가 정해지면 보낸다.
  struct APend { std::vector<float> pcm; double rel; };   // rel: t0 기준 재생 시각(초)
  std::vector<APend> apend;
  uint64_t a_anchor_v = 0;   // 현재 회차 시작 영상 순번
  int64_t a_samples = 0;     // 현재 회차에서 지금까지 만든 음성 샘플 수
  double a_off = 0;          // 음성 시작이 영상보다 늦은/빠른 정도(초) - 첫 회차만 의미 있음
  if (ast && st->start_time != AV_NOPTS_VALUE && ast->start_time != AV_NOPTS_VALUE)
    a_off = ast->start_time * av_q2d(ast->time_base) - st->start_time * av_q2d(st->time_base);
  auto flush_audio = [&]() {
    if (shown == 0) return;
    for (auto& a : apend) {
      const auto due = t0 + std::chrono::duration_cast<clk::duration>(std::chrono::duration<double>(a.rel));
      audio_->PushAt(a.pcm.data(), (int)(a.pcm.size() / 2), due);
    }
    apend.clear();
  };
  auto handle_audio = [&](AVPacket* p) {
    if (avcodec_send_packet(adec, p) < 0) return;
    AVFrame* af = av_frame_alloc();
    while (avcodec_receive_frame(adec, af) == 0) {
      const int cap = swr_get_out_samples(swr, af->nb_samples);
      std::vector<float> pcm((size_t)std::max(cap, 0) * 2);
      uint8_t* o = (uint8_t*)pcm.data();
      const int got = swr_convert(swr, &o, cap, (const uint8_t**)af->extended_data, af->nb_samples);
      if (got <= 0) continue;
      pcm.resize((size_t)got * 2);
      const double rel = a_anchor_v / fps + a_off + (double)a_samples / AudioMixer::kRate;
      a_samples += got;
      apend.push_back({std::move(pcm), rel});
    }
    av_frame_free(&af);
    flush_audio();
  };

  // 디코더 출력 프레임 1개 처리. 원본 fps 에 맞춰 대기 후 현재 프레임으로 교체.
  auto handle_frame = [&](MppFrame f) {
    if (mpp_frame_get_info_change(f)) {   // 해상도/버퍼 정보 확정 -> 외부 버퍼 그룹 연결
      {
        std::lock_guard<std::mutex> lk(mu_);
        if (!grp_) mpp_buffer_group_get_internal(&grp_, (MppBufferType)(MPP_BUFFER_TYPE_DRM | MPP_BUFFER_FLAGS_DMA32));
      }
      mpi->control(ctx, MPP_DEC_SET_EXT_BUF_GROUP, grp_);
      mpi->control(ctx, MPP_DEC_SET_INFO_CHANGE_READY, nullptr);
      printf("[video] %ux%u stride=%ux%u fmt=0x%x\n", mpp_frame_get_width(f), mpp_frame_get_height(f),
             mpp_frame_get_hor_stride(f), mpp_frame_get_ver_stride(f), (unsigned)mpp_frame_get_fmt(f));
      mpp_frame_deinit(&f);
      return;
    }
    if (mpp_frame_get_errinfo(f) || mpp_frame_get_discard(f) || !mpp_frame_get_buffer(f) ||
        (mpp_frame_get_fmt(f) & MPP_FRAME_FMT_MASK) != MPP_FMT_YUV420SP) {   // NV12 만 합성
      mpp_frame_deinit(&f);
      return;
    }
    if (shown == 0) t0 = clk::now();
    auto due = t0 + std::chrono::duration_cast<clk::duration>(std::chrono::duration<double>(shown / fps));
    if (clk::now() - due > std::chrono::milliseconds(500)) {   // 크게 밀리면 시계 재동기
      t0 = clk::now() - std::chrono::duration_cast<clk::duration>(std::chrono::duration<double>(shown / fps));
      due = clk::now();
    }
    std::this_thread::sleep_until(due);
    Publish(f);
    shown++;
    if (shown == 1 && audio_) flush_audio();
  };
  auto drain_one = [&]() -> bool {
    MppFrame f = nullptr;
    if (mpi->decode_get_frame(ctx, &f) == MPP_OK && f) { handle_frame(f); return true; }
    return false;
  };
  auto put = [&](AVPacket* p) {
    MppPacket mp = nullptr;
    mpp_packet_init(&mp, p->data, p->size);
    mpp_packet_set_pts(mp, p->pts);
    while (!stop_) {
      if (mpi->decode_put_packet(ctx, mp) == MPP_OK) break;   // 입력 큐가 차면 출력부터 소비
      if (!drain_one()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    mpp_packet_deinit(&mp);
    vpkts++;
  };

  pkt = av_packet_alloc();
  int idle = 0;
  bool eof = false;
  while (!stop_) {
    if (drain_one()) { idle = 0; continue; }
    if (eof) {
      if (loop) {   // 처음부터 다시 (디코더 상태는 유지, IDR 부터 이어짐)
        av_seek_frame(fc, si, 0, AVSEEK_FLAG_BACKWARD);
        if (bsf) av_bsf_flush(bsf);
        if (adec) {
          avcodec_flush_buffers(adec);
          a_anchor_v = vpkts;
          a_samples = 0;
          a_off = 0;
        }
        eof = false;
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (++idle > 100) break;   // 남은 프레임 출력 대기 후 종료
      }
      continue;
    }
    // 영상 패킷 1개를 넣을 때까지 읽는다 (사이사이의 음성 패킷은 바로 처리해 음성이 밀리지 않게 함)
    bool got_video = false;
    while (!stop_ && !got_video) {
      if (av_read_frame(fc, pkt) < 0) { eof = true; break; }
      if (pkt->stream_index == asi && adec) {
        handle_audio(pkt);
      } else if (pkt->stream_index == si) {
        if (bsf) {
          if (av_bsf_send_packet(bsf, pkt) >= 0)
            while (av_bsf_receive_packet(bsf, pkt) == 0) { put(pkt); av_packet_unref(pkt); }
        } else {
          put(pkt);
        }
        got_video = true;
      }
      av_packet_unref(pkt);
    }
  }

  av_packet_free(&pkt);
  swr_free(&swr);
  avcodec_free_context(&adec);
  if (bsf) av_bsf_free(&bsf);
  avformat_close_input(&fc);
  mpi->reset(ctx);
  mpp_destroy(ctx);   // 현재 프레임(cur_)과 버퍼 그룹은 Stop() 에서 해제
  running_ = false;
  printf("[video] stop\n");
}
