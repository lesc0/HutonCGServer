#pragma once
// CG_DRM_OUT 빌드 전용: 송출 모니터(기본 HDMI-2)에 X 창 없이 DRM/KMS 로 NV12 프레임을 직접 출력한다.
//  1) X 서버(RandR)에게 그 모니터의 출력(connector)+CRTC 를 DRM lease 로 빌려 와 별도의 DRM master fd 를 얻는다
//     (X 가 DRM master 를 쥐고 있어 그냥 열면 modeset 이 안 되기 때문. HDMI-1 데스크탑은 X 가 그대로 쓴다)
//  2) libdrm atomic 으로 권장 모드 modeset + NV12 plane 에 FB 를 번갈아 flip 한다(더블버퍼).
// 입력(마우스/키보드)은 받지 않는다.
#ifdef CG_DRM_OUT
#include <cstdint>
#include <functional>
#include <string>

#include "dma_heap_buf.h"

struct xcb_connection_t;

class DrmOut {
 public:
  using FillFn = std::function<bool(int dst_fd, int w, int h, int hor_stride, int ver_stride)>;

  ~DrmOut() { Close(); }
  // output: RandR 출력 이름(예: "HDMI-2"). 반환: 0 성공, 1 실패(이유는 stderr), 2 모니터가 연결되어 있지 않음
  int Open(const std::string& output);
  // 빈 버퍼에 fill 로 NV12 프레임을 채운 뒤 화면에 올린다(vblank 에 반영될 때까지 블록).
  bool Present(const FillFn& fill);
  void Close();
  int width() const { return mode_w_; }
  int height() const { return mode_h_; }

 private:
  struct Fb {
    DmaHeapBuf buf;
    uint32_t handle = 0, id = 0;
  };
  bool Lease(const std::string& output, int& rc);
  bool Setup();
  bool AllocFbs();

  xcb_connection_t* xcb_ = nullptr;
  int fd_ = -1;                       // lease 로 받은 DRM fd
  uint32_t conn_ = 0, crtc_ = 0, plane_ = 0, mode_blob_ = 0;
  uint32_t p_conn_crtc_ = 0, p_crtc_mode_ = 0, p_crtc_active_ = 0;
  uint32_t p_fb_ = 0, p_plane_crtc_ = 0, p_sx_ = 0, p_sy_ = 0, p_sw_ = 0, p_sh_ = 0, p_dx_ = 0, p_dy_ = 0, p_dw_ = 0, p_dh_ = 0;
  int mode_w_ = 0, mode_h_ = 0, hor_ = 0, ver_ = 0;
  uint8_t mode_[68] = {};             // drmModeModeInfo (포함 헤더를 .cpp 로 숨기려고 바이트 배열로 보관)
  Fb fbs_[2];
  int cur_ = 0;
  bool first_ = true;
};
#endif
