#include "mpp_encoder.h"

#include <rga/RgaApi.h>
#include <rga/im2d.hpp>

#include <linux/dma-buf.h>
#include <poll.h>
#include <sys/ioctl.h>

#include <algorithm>
#include <cstdio>
#include <chrono>
#include <cstring>

bool MppH264Encoder::Init(int width, int height, int fps, int bitrate_bps) {
  w_ = width;
  h_ = height;
  hor_ = (w_ + 15) & ~15;  // 16 정렬 stride (1080 -> 1088)
  ver_ = (h_ + 15) & ~15;

  // DMA32: RGA 가 fd 로 직접 읽고 쓸 수 있도록 4GB 이하 메모리에서 할당
  if (mpp_buffer_group_get_internal(&grp_, (MppBufferType)(MPP_BUFFER_TYPE_DRM | MPP_BUFFER_FLAGS_DMA32)) != MPP_OK) {
    fprintf(stderr, "[mpp] buffer group failed\n");
    return false;
  }
  for (auto& b : bufs_) {
    if (mpp_buffer_get(grp_, &b, (size_t)hor_ * ver_ * 3 / 2) != MPP_OK) {
      fprintf(stderr, "[mpp] buffer alloc failed\n");
      return false;
    }
  }
  // 영상 합성용 캔버스/UI 버퍼 (실패해도 기존 OnPaint 경로는 동작)
  if (!canvas_.Alloc((size_t)w_ * h_ * 4) || !ui_.Alloc((size_t)w_ * h_ * 4) || !ui_.Map())
    fprintf(stderr, "[rga] 합성 버퍼 할당 실패 - 영상 합성 비활성\n");

  if (mpp_create(&ctx_, &mpi_) != MPP_OK ||
      mpp_init(ctx_, MPP_CTX_ENC, MPP_VIDEO_CodingAVC) != MPP_OK) {
    fprintf(stderr, "[mpp] init failed\n");
    return false;
  }

  mpp_enc_cfg_init(&cfg_);
  mpi_->control(ctx_, MPP_ENC_GET_CFG, cfg_);

  mpp_enc_cfg_set_s32(cfg_, "prep:width", w_);
  mpp_enc_cfg_set_s32(cfg_, "prep:height", h_);
  mpp_enc_cfg_set_s32(cfg_, "prep:hor_stride", hor_);
  mpp_enc_cfg_set_s32(cfg_, "prep:ver_stride", ver_);
  mpp_enc_cfg_set_s32(cfg_, "prep:format", MPP_FMT_YUV420SP);

  mpp_enc_cfg_set_s32(cfg_, "rc:mode", MPP_ENC_RC_MODE_CBR);
  mpp_enc_cfg_set_s32(cfg_, "rc:fps_in_flex", 0);
  mpp_enc_cfg_set_s32(cfg_, "rc:fps_in_num", fps);  // cgsetup.cfg: fps=... 로 조정
  mpp_enc_cfg_set_s32(cfg_, "rc:fps_in_denorm", 1);
  mpp_enc_cfg_set_s32(cfg_, "rc:fps_out_flex", 0);
  mpp_enc_cfg_set_s32(cfg_, "rc:fps_out_num", fps);
  mpp_enc_cfg_set_s32(cfg_, "rc:fps_out_denorm", 1);
  mpp_enc_cfg_set_s32(cfg_, "rc:gop", fps);  // IDR 1초 간격
  mpp_enc_cfg_set_s32(cfg_, "rc:bps_target", bitrate_bps);
  mpp_enc_cfg_set_s32(cfg_, "rc:bps_max", bitrate_bps * 17 / 16);
  mpp_enc_cfg_set_s32(cfg_, "rc:bps_min", bitrate_bps * 15 / 16);

  mpp_enc_cfg_set_s32(cfg_, "codec:type", MPP_VIDEO_CodingAVC);
  mpp_enc_cfg_set_s32(cfg_, "h264:profile", 100);  // High
  mpp_enc_cfg_set_s32(cfg_, "h264:level", 42);     // 1080p60
  mpp_enc_cfg_set_s32(cfg_, "h264:cabac_en", 1);
  mpp_enc_cfg_set_s32(cfg_, "h264:cabac_idc", 0);
  mpp_enc_cfg_set_s32(cfg_, "h264:trans8x8", 1);

  if (mpi_->control(ctx_, MPP_ENC_SET_CFG, cfg_) != MPP_OK) {
    fprintf(stderr, "[mpp] set cfg failed\n");
    return false;
  }
  // 매 IDR마다 SPS/PPS 포함 (스트림 중간 진입 가능)
  MppEncHeaderMode hm = MPP_ENC_HEADER_MODE_EACH_IDR;
  mpi_->control(ctx_, MPP_ENC_SET_HEADER_MODE, &hm);
  return true;
}

int MppH264Encoder::AcquireWrite() {
  std::lock_guard<std::mutex> lk(mu_);
  for (int i = 0; i < 3; i++)
    if (i != ready_ && i != encoding_) return i;
  return 0;  // 도달하지 않음 (버퍼 3개, 점유 최대 2개)
}

bool MppH264Encoder::Convert(const uint8_t* bgra) {
  if (!bufs_[0]) return false;
  int idx = AcquireWrite();
  rga_buffer_t src = wrapbuffer_virtualaddr((void*)bgra, w_, h_, RK_FORMAT_BGRA_8888);
  rga_buffer_t dst = wrapbuffer_virtualaddr(mpp_buffer_get_ptr(bufs_[idx]), w_, h_,
                                            RK_FORMAT_YCbCr_420_SP, hor_, ver_);
  IM_STATUS s = imcvtcolor(src, dst, RK_FORMAT_BGRA_8888, RK_FORMAT_YCbCr_420_SP);
  if (s != IM_STATUS_SUCCESS) {
    fprintf(stderr, "[rga] %s\n", imStrError(s));
    return false;
  }
  std::lock_guard<std::mutex> lk(mu_);
  ready_ = idx;
  return true;
}

bool MppH264Encoder::ConvertDmaBuf(int fd, int stride_bytes, bool bgra) {
  if (!bufs_[0] || fd < 0 || stride_bytes < w_ * 4) return false;
  int idx = AcquireWrite();
  const int src_fmt = bgra ? RK_FORMAT_BGRA_8888 : RK_FORMAT_RGBA_8888;

  // GPU 쓰기 완료 대기(암묵적 동기화) + 캐시 동기화
  pollfd pfd{fd, POLLIN, 0};
  poll(&pfd, 1, 20);
  dma_buf_sync sync{DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
  ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);

  rga_buffer_handle_t h = importbuffer_fd(fd, stride_bytes * h_);
  bool ok = false;
  if (h) {
    // wstride 는 픽셀 단위
    rga_buffer_t src = wrapbuffer_handle(h, w_, h_, src_fmt, stride_bytes / 4, h_);
    rga_buffer_t dst = wrapbuffer_virtualaddr(mpp_buffer_get_ptr(bufs_[idx]), w_, h_,
                                              RK_FORMAT_YCbCr_420_SP, hor_, ver_);
    IM_STATUS s = imcvtcolor(src, dst, src_fmt, RK_FORMAT_YCbCr_420_SP);
    ok = (s == IM_STATUS_SUCCESS);
    if (!ok) fprintf(stderr, "[rga] dmabuf: %s\n", imStrError(s));
    releasebuffer_handle(h);
  } else {
    fprintf(stderr, "[rga] importbuffer_fd failed (fd=%d)\n", fd);
  }

  sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
  ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);

  if (ok) {
    std::lock_guard<std::mutex> lk(mu_);
    ready_ = idx;
  }
  return ok;
}

bool MppH264Encoder::UploadUi(const uint8_t* bgra) {
  if (!ui_.ptr) return false;
  ui_.BeginCpuWrite();
  memcpy(ui_.ptr, bgra, (size_t)w_ * h_ * 4);
  ui_.EndCpuWrite();
  ui_valid_ = true;
  return true;
}

double MppH264Encoder::TakeComposeMs() {
  const double r = compose_n_ ? compose_ms_sum_ / compose_n_ : 0;
  compose_ms_sum_ = 0;
  compose_n_ = 0;
  return r;
}

bool MppH264Encoder::Compose(const VideoFrameRef* v, int tx, int ty, int tw, int th) {
  if (!bufs_[0] || canvas_.fd < 0) return false;
  const auto t0 = std::chrono::steady_clock::now();
  int idx = AcquireWrite();
  rga_buffer_t canvas = wrapbuffer_fd(canvas_.fd, w_, h_, RK_FORMAT_BGRA_8888);
  const im_rect full = {0, 0, w_, h_};
  IM_STATUS s;

  // 1) 영상 -> 캔버스: 대상 영역(tx,ty,tw,th) 안에 비율 유지로 맞춤, 나머지는 검정
  im_rect dr = full;
  if (v) {
    const double sc = std::min((double)tw / v->width, (double)th / v->height);
    dr.width = (int)(v->width * sc) & ~1;
    dr.height = (int)(v->height * sc) & ~1;
    dr.x = (tx + (tw - dr.width) / 2) & ~1;
    dr.y = (ty + (th - dr.height) / 2) & ~1;
  }
  // 캔버스에는 영상만 둔다. 영상 영역 밖의 검정은 영역이 바뀔 때만 다시 채움 (4K 입력 시 RGA 부하 절감)
  const int key[4] = {v ? dr.x : 0, v ? dr.y : 0, v ? dr.width : 0, v ? dr.height : 0};
  if (!std::equal(key, key + 4, canvas_key_)) {
    if (!v || dr.x != 0 || dr.y != 0 || dr.width != w_ || dr.height != h_) {
      s = imfill(canvas, full, 0);
      if (s != IM_STATUS_SUCCESS) { fprintf(stderr, "[rga] fill: %s\n", imStrError(s)); return false; }
    }
    std::copy(key, key + 4, canvas_key_);
  }
  if (v) {
    const int sfmt = v->format ? v->format : RK_FORMAT_YCbCr_420_SP;   // MPP 디코더=NV12, HDMI 입력=BGR888/NV12/16/24
    rga_buffer_t src = wrapbuffer_fd(v->fd, v->width, v->height, sfmt, v->hor_stride, v->ver_stride);
    rga_buffer_t pat{};
    s = improcess(src, canvas, pat, {0, 0, v->width, v->height}, dr, {}, -1, nullptr, nullptr, IM_SYNC);
    if (s != IM_STATUS_SUCCESS) { fprintf(stderr, "[rga] video: %s\n", imStrError(s)); return false; }
  }
  // 2) UI(프리멀티플라이드 알파) + 캔버스 -> 인코더 입력 NV12
  rga_buffer_t dst = wrapbuffer_fd(mpp_buffer_get_fd(bufs_[idx]), w_, h_, RK_FORMAT_YCbCr_420_SP, hor_, ver_);
  bool done = false;
  if (ui_valid_) {
    rga_buffer_t ui = wrapbuffer_fd(ui_.fd, w_, h_, RK_FORMAT_BGRA_8888);
    if (composite_ok_) {   // 한 번에: dst = UI over 캔버스 (캔버스는 그대로 유지)
      s = imcomposite(ui, canvas, dst, IM_ALPHA_BLEND_SRC_OVER | IM_ALPHA_BLEND_PRE_MUL);
      if (s == IM_STATUS_SUCCESS) done = true;
      else {
        composite_ok_ = false;
        fprintf(stderr, "[rga] composite->NV12 미지원(%s) - blend+변환 방식 사용\n", imStrError(s));
      }
    }
    if (!done) {           // 대체: 캔버스에 직접 합성 -> 다음 프레임에 캔버스 다시 채움
      s = imblend(ui, canvas, IM_ALPHA_BLEND_SRC_OVER | IM_ALPHA_BLEND_PRE_MUL);
      if (s != IM_STATUS_SUCCESS) { fprintf(stderr, "[rga] blend: %s\n", imStrError(s)); return false; }
      canvas_key_[0] = -1;
    }
  }
  if (!done) {
    s = imcvtcolor(canvas, dst, RK_FORMAT_BGRA_8888, RK_FORMAT_YCbCr_420_SP);
    if (s != IM_STATUS_SUCCESS) { fprintf(stderr, "[rga] nv12: %s\n", imStrError(s)); return false; }
  }
  compose_ms_sum_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  compose_n_++;

  std::lock_guard<std::mutex> lk(mu_);
  ready_ = idx;
  return true;
}

bool MppH264Encoder::Encode(const PacketCb& cb) {
  int idx;
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (ready_ < 0) return false;   // 아직 변환된 프레임 없음
    idx = ready_;
    encoding_ = idx;
    last_idx_ = idx;
  }

  MppFrame frame = nullptr;
  mpp_frame_init(&frame);
  mpp_frame_set_width(frame, w_);
  mpp_frame_set_height(frame, h_);
  mpp_frame_set_hor_stride(frame, hor_);
  mpp_frame_set_ver_stride(frame, ver_);
  mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);
  mpp_frame_set_eos(frame, 0);
  mpp_frame_set_buffer(frame, bufs_[idx]);

  MPP_RET ret = mpi_->encode_put_frame(ctx_, frame);
  mpp_frame_deinit(&frame);

  bool ok = false;
  if (ret == MPP_OK) {
    MppPacket pkt = nullptr;
    if (mpi_->encode_get_packet(ctx_, &pkt) == MPP_OK && pkt) {
      cb((const uint8_t*)mpp_packet_get_pos(pkt), mpp_packet_get_length(pkt));
      mpp_packet_deinit(&pkt);
      ok = true;
    }
  }
  std::lock_guard<std::mutex> lk(mu_);
  encoding_ = -1;
  return ok;
}

bool MppH264Encoder::ExportPreviewBgrx(uint8_t* out, int out_w, int out_h) {
  int idx;
  { std::lock_guard<std::mutex> lk(mu_); idx = last_idx_; }
  if (idx < 0 || !bufs_[idx]) return false;
  rga_buffer_t src = wrapbuffer_fd(mpp_buffer_get_fd(bufs_[idx]), w_, h_, RK_FORMAT_YCbCr_420_SP, hor_, ver_);
  rga_buffer_t dst = wrapbuffer_virtualaddr(out, out_w, out_h, RK_FORMAT_BGRX_8888);
  rga_buffer_t pat{};
  IM_STATUS s = improcess(src, dst, pat, {0, 0, w_, h_}, {0, 0, out_w, out_h}, {}, -1, nullptr, nullptr, IM_SYNC);
  if (s != IM_STATUS_SUCCESS) { fprintf(stderr, "[rga] preview: %s\n", imStrError(s)); return false; }
  return true;
}

#ifdef CG_DRM_OUT
bool MppH264Encoder::ExportPreviewNv12(int dst_fd, int out_w, int out_h, int hor_stride, int ver_stride) {
  int idx;
  { std::lock_guard<std::mutex> lk(mu_); idx = last_idx_; }
  if (idx < 0 || !bufs_[idx] || dst_fd < 0) return false;
  rga_buffer_t src = wrapbuffer_fd(mpp_buffer_get_fd(bufs_[idx]), w_, h_, RK_FORMAT_YCbCr_420_SP, hor_, ver_);
  rga_buffer_t dst = wrapbuffer_fd(dst_fd, out_w, out_h, RK_FORMAT_YCbCr_420_SP, hor_stride, ver_stride);
  rga_buffer_t pat{};
  IM_STATUS s = improcess(src, dst, pat, {0, 0, w_, h_}, {0, 0, out_w, out_h}, {}, -1, nullptr, nullptr, IM_SYNC);
  if (s != IM_STATUS_SUCCESS) { fprintf(stderr, "[rga] drm preview: %s\n", imStrError(s)); return false; }
  return true;
}
#endif

void MppH264Encoder::Reset() {
  std::lock_guard<std::mutex> lk(mu_);
  ready_ = -1;
}

void MppH264Encoder::Deinit() {
  if (ctx_) { mpp_destroy(ctx_); ctx_ = nullptr; mpi_ = nullptr; }
  if (cfg_) { mpp_enc_cfg_deinit(cfg_); cfg_ = nullptr; }
  for (auto& b : bufs_)
    if (b) { mpp_buffer_put(b); b = nullptr; }
  if (grp_) { mpp_buffer_group_put(grp_); grp_ = nullptr; }
  canvas_.Free();
  ui_.Free();
  ui_valid_ = false;
  canvas_key_[0] = -1;
  composite_ok_ = true;
  ready_ = encoding_ = last_idx_ = -1;
}
