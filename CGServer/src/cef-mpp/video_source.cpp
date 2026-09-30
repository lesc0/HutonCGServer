#include "video_source.h"

#include <pthread.h>

#include <chrono>
#include <cstdio>
#include <functional>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

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
        eof = false;
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (++idle > 100) break;   // 남은 프레임 출력 대기 후 종료
      }
      continue;
    }
    if (av_read_frame(fc, pkt) < 0) { eof = true; continue; }
    if (pkt->stream_index != si) { av_packet_unref(pkt); continue; }
    if (bsf) {
      if (av_bsf_send_packet(bsf, pkt) < 0) { av_packet_unref(pkt); continue; }
      while (av_bsf_receive_packet(bsf, pkt) == 0) { put(pkt); av_packet_unref(pkt); }
    } else {
      put(pkt);
      av_packet_unref(pkt);
    }
  }

  av_packet_free(&pkt);
  if (bsf) av_bsf_free(&bsf);
  avformat_close_input(&fc);
  mpi->reset(ctx);
  mpp_destroy(ctx);   // 현재 프레임(cur_)과 버퍼 그룹은 Stop() 에서 해제
  running_ = false;
  printf("[video] stop\n");
}
