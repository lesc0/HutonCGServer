#ifdef CG_DRM_OUT
#include "drm_out.h"

#include <drm_fourcc.h>
#include <xcb/randr.h>
#include <xcb/xcb.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

static_assert(sizeof(drmModeModeInfo) == 68, "drm_out.h 의 mode_ 크기 확인");

static uint32_t PropId(int fd, uint32_t obj, uint32_t type, const char* name) {
  uint32_t id = 0;
  drmModeObjectProperties* ps = drmModeObjectGetProperties(fd, obj, type);
  if (!ps) return 0;
  for (uint32_t i = 0; i < ps->count_props && !id; i++) {
    drmModePropertyRes* p = drmModeGetProperty(fd, ps->props[i]);
    if (p) {
      if (!strcmp(p->name, name)) id = p->prop_id;
      drmModeFreeProperty(p);
    }
  }
  drmModeFreeObjectProperties(ps);
  return id;
}

// X(RandR) 에서 output 이름의 출력을 찾아 CRTC 와 함께 DRM lease 를 받는다. rc: 2 = 모니터 없음
bool DrmOut::Lease(const std::string& output, int& rc) {
  rc = 1;
  int scr = 0;
  xcb_ = xcb_connect(nullptr, &scr);
  if (!xcb_ || xcb_connection_has_error(xcb_)) { fprintf(stderr, "[drm] X 서버(xcb) 연결 실패 - X 없이 DRM 을 직접 엽니다\n"); rc = 3; return false; }
  xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(xcb_));
  for (; it.rem && scr > 0; scr--) xcb_screen_next(&it);
  if (!it.rem) { fprintf(stderr, "[drm] X 화면을 찾지 못함\n"); return false; }
  const xcb_window_t root = it.data->root;

  struct Found { xcb_randr_output_t out = 0; bool connected = false; xcb_randr_crtc_t cur_crtc = 0; std::vector<xcb_randr_crtc_t> crtcs; };
  auto query = [&](Found& f) -> bool {
    f = Found{};
    xcb_randr_get_screen_resources_current_reply_t* res =
        xcb_randr_get_screen_resources_current_reply(xcb_, xcb_randr_get_screen_resources_current(xcb_, root), nullptr);
    if (!res) { fprintf(stderr, "[drm] RandR 화면 정보를 읽지 못함\n"); return false; }
    const xcb_randr_output_t* outs = xcb_randr_get_screen_resources_current_outputs(res);
    const int n = xcb_randr_get_screen_resources_current_outputs_length(res);
    for (int i = 0; i < n; i++) {
      xcb_randr_get_output_info_reply_t* oi = xcb_randr_get_output_info_reply(xcb_, xcb_randr_get_output_info(xcb_, outs[i], XCB_CURRENT_TIME), nullptr);
      if (!oi) continue;
      const std::string name((const char*)xcb_randr_get_output_info_name(oi), xcb_randr_get_output_info_name_length(oi));
      if (name == output) {
        f.out = outs[i];
        f.connected = oi->connection == XCB_RANDR_CONNECTION_CONNECTED;
        f.cur_crtc = oi->crtc;
        const xcb_randr_crtc_t* cs = xcb_randr_get_output_info_crtcs(oi);
        f.crtcs.assign(cs, cs + xcb_randr_get_output_info_crtcs_length(oi));
      }
      free(oi);
    }
    free(res);
    return true;
  };

  Found f;
  if (!query(f)) return false;
  if (!f.out) { fprintf(stderr, "[drm] RandR 에 출력 '%s' 이 없음\n", output.c_str()); return false; }
  fprintf(stderr, "[drm] RandR %s: connected=%d crtc=%u 가능한 crtc=%zu개\n", output.c_str(), (int)f.connected, (unsigned)f.cur_crtc, f.crtcs.size());
  if (!f.connected) { rc = 2; return false; }
  if (f.cur_crtc) {   // X 가 이 출력을 쓰고 있으면 lease 할 수 없으므로 끈다
    printf("[drm] %s 를 X 에서 끕니다(DRM 직접 출력)\n", output.c_str());
    const std::string cmd = "xrandr --output " + output + " --off >/dev/null 2>&1";
    if (system(cmd.c_str()) != 0) fprintf(stderr, "[drm] xrandr --off 실행 실패\n");
    for (int i = 0; i < 20; i++) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      if (!query(f)) return false;
      if (!f.cur_crtc) break;
    }
    if (f.cur_crtc) { fprintf(stderr, "[drm] %s 가 X 에서 꺼지지 않음\n", output.c_str()); return false; }
  }
  xcb_randr_crtc_t crtc = 0;   // 가능한 CRTC 중 X 가 쓰고 있지 않은(연결된 출력이 없는) 것
  for (xcb_randr_crtc_t c : f.crtcs) {
    xcb_randr_get_crtc_info_reply_t* ci = xcb_randr_get_crtc_info_reply(xcb_, xcb_randr_get_crtc_info(xcb_, c, XCB_CURRENT_TIME), nullptr);
    if (ci && xcb_randr_get_crtc_info_outputs_length(ci) == 0) crtc = c;
    free(ci);
    if (crtc) break;
  }
  fprintf(stderr, "[drm] lease 요청: crtc=%u output=%u\n", (unsigned)crtc, (unsigned)f.out);
  if (!crtc) { fprintf(stderr, "[drm] 쓸 수 있는 빈 CRTC 가 없음\n"); return false; }

  xcb_generic_error_t* err = nullptr;
  xcb_randr_lease_t lid = xcb_generate_id(xcb_);
  xcb_randr_create_lease_reply_t* lr = xcb_randr_create_lease_reply(xcb_, xcb_randr_create_lease(xcb_, root, lid, 1, 1, &crtc, &f.out), &err);
  if (!lr || lr->nfd < 1) {
    fprintf(stderr, "[drm] RandR lease 생성 실패 (error_code=%d) - X 서버가 lease 를 지원하는지(modesetting 드라이버, Xorg 1.20+) 확인\n", err ? err->error_code : -1);
    free(err);
    free(lr);
    return false;
  }
  fd_ = xcb_randr_create_lease_reply_fds(xcb_, lr)[0];
  free(lr);
  return true;
}

// X 가 없을 때: DRM 장치를 직접 열어 master 가 되고, output("HDMI-N") 에 해당하는 connector 와 그 CRTC 를 고른다. rc: 2 = 모니터 없음
bool DrmOut::OpenDirect(const std::string& output, int& rc) {
  rc = 1;
  int n = 0;
  if (output.size() >= 6 && output.compare(0, 5, "HDMI-") == 0) n = atoi(output.c_str() + 5);
  if (n < 1) { fprintf(stderr, "[drm] 출력 이름 '%s' 을 HDMI-N 으로 해석할 수 없음\n", output.c_str()); return false; }
  const char* dev = getenv("CG_DRM_DEVICE");
  fd_ = open(dev && *dev ? dev : "/dev/dri/card0", O_RDWR | O_CLOEXEC);
  if (fd_ < 0) { perror("[drm] /dev/dri/card0 열기"); return false; }
  if (drmSetMaster(fd_) != 0) { fprintf(stderr, "[drm] DRM master 를 얻지 못함 - X(lightdm) 등 다른 프로세스가 화면을 쥐고 있음\n"); return false; }
  drmSetClientCap(fd_, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
  if (drmSetClientCap(fd_, DRM_CLIENT_CAP_ATOMIC, 1) != 0) { fprintf(stderr, "[drm] atomic 미지원\n"); return false; }
  drmModeRes* res = drmModeGetResources(fd_);
  if (!res) { fprintf(stderr, "[drm] drmModeGetResources 실패\n"); return false; }
  drmModeConnector* conn = nullptr;
  for (int i = 0; i < res->count_connectors && !conn; i++) {
    drmModeConnector* c = drmModeGetConnector(fd_, res->connectors[i]);
    if (c && c->connector_type == DRM_MODE_CONNECTOR_HDMIA && (int)c->connector_type_id == n) conn = c;
    else if (c) drmModeFreeConnector(c);
  }
  if (!conn) { fprintf(stderr, "[drm] connector HDMI-A-%d 이 없음\n", n); drmModeFreeResources(res); return false; }
  if (conn->connection != DRM_MODE_CONNECTED) { rc = 2; drmModeFreeConnector(conn); drmModeFreeResources(res); return false; }
  conn_ = conn->connector_id;
  crtc_ = 0;
  uint32_t possible = 0;   // 현재 encoder 가 쓰는 CRTC 가 있으면 그것, 없으면 모든 encoder 가 쓸 수 있는 CRTC 의 합집합
  for (int i = 0; i < conn->count_encoders; i++) {
    drmModeEncoder* e = drmModeGetEncoder(fd_, conn->encoders[i]);
    if (!e) continue;
    if (conn->encoder_id == e->encoder_id && e->crtc_id) crtc_ = e->crtc_id;
    possible |= e->possible_crtcs;
    drmModeFreeEncoder(e);
  }
  crtc_idx_ = -1;
  for (int i = 0; i < res->count_crtcs; i++) {
    if (crtc_ ? res->crtcs[i] == crtc_ : ((possible >> i) & 1u)) { crtc_idx_ = i; crtc_ = res->crtcs[i]; break; }
  }
  drmModeFreeConnector(conn);
  drmModeFreeResources(res);
  if (crtc_idx_ < 0) { fprintf(stderr, "[drm] 쓸 수 있는 CRTC 가 없음\n"); return false; }
  direct_ = true;
  printf("[drm] DRM 직접 열기: HDMI-A-%d connector %u crtc %u\n", n, conn_, crtc_);
  return true;
}

bool DrmOut::Setup() {
  drmSetClientCap(fd_, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
  if (drmSetClientCap(fd_, DRM_CLIENT_CAP_ATOMIC, 1) != 0) { fprintf(stderr, "[drm] atomic 미지원\n"); return false; }
  if (!direct_) {   // lease: 빌려 온 connector/crtc 는 하나씩뿐
    drmModeRes* res = drmModeGetResources(fd_);
    if (!res || res->count_connectors < 1 || res->count_crtcs < 1) {
      fprintf(stderr, "[drm] lease 에 connector/crtc 가 없음\n");
      if (res) drmModeFreeResources(res);
      return false;
    }
    conn_ = res->connectors[0];
    crtc_ = res->crtcs[0];
    crtc_idx_ = 0;
    drmModeFreeResources(res);
  }

  drmModeConnector* c = drmModeGetConnector(fd_, conn_);
  if (!c || c->connection != DRM_MODE_CONNECTED || c->count_modes < 1) {
    fprintf(stderr, "[drm] connector 가 연결되어 있지 않거나 모드가 없음\n");
    if (c) drmModeFreeConnector(c);
    return false;
  }
  int pick = 0;   // 권장(PREFERRED) 모드 우선, 없으면 첫 번째
  for (int i = 0; i < c->count_modes; i++)
    if (c->modes[i].type & DRM_MODE_TYPE_PREFERRED) { pick = i; break; }
  memcpy(mode_, &c->modes[pick], sizeof(drmModeModeInfo));
  mode_w_ = c->modes[pick].hdisplay;
  mode_h_ = c->modes[pick].vdisplay;
  printf("[drm] connector %u crtc %u 모드 %s %dx%d@%d\n", conn_, crtc_, c->modes[pick].name, mode_w_, mode_h_, c->modes[pick].vrefresh);
  drmModeFreeConnector(c);

  drmModePlaneRes* pr = drmModeGetPlaneResources(fd_);
  if (pr) {
    for (uint32_t i = 0; i < pr->count_planes && !plane_; i++) {
      drmModePlane* p = drmModeGetPlane(fd_, pr->planes[i]);
      if (!p) continue;
      if (p->possible_crtcs & (1u << crtc_idx_))
        for (uint32_t k = 0; k < p->count_formats; k++)
          if (p->formats[k] == DRM_FORMAT_NV12) { plane_ = p->plane_id; break; }
      drmModeFreePlane(p);
    }
    drmModeFreePlaneResources(pr);
  }
  if (!plane_) { fprintf(stderr, "[drm] NV12 를 받는 plane 이 lease 에 없음\n"); return false; }

  p_conn_crtc_ = PropId(fd_, conn_, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");
  p_crtc_mode_ = PropId(fd_, crtc_, DRM_MODE_OBJECT_CRTC, "MODE_ID");
  p_crtc_active_ = PropId(fd_, crtc_, DRM_MODE_OBJECT_CRTC, "ACTIVE");
  p_fb_ = PropId(fd_, plane_, DRM_MODE_OBJECT_PLANE, "FB_ID");
  p_plane_crtc_ = PropId(fd_, plane_, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
  p_sx_ = PropId(fd_, plane_, DRM_MODE_OBJECT_PLANE, "SRC_X");
  p_sy_ = PropId(fd_, plane_, DRM_MODE_OBJECT_PLANE, "SRC_Y");
  p_sw_ = PropId(fd_, plane_, DRM_MODE_OBJECT_PLANE, "SRC_W");
  p_sh_ = PropId(fd_, plane_, DRM_MODE_OBJECT_PLANE, "SRC_H");
  p_dx_ = PropId(fd_, plane_, DRM_MODE_OBJECT_PLANE, "CRTC_X");
  p_dy_ = PropId(fd_, plane_, DRM_MODE_OBJECT_PLANE, "CRTC_Y");
  p_dw_ = PropId(fd_, plane_, DRM_MODE_OBJECT_PLANE, "CRTC_W");
  p_dh_ = PropId(fd_, plane_, DRM_MODE_OBJECT_PLANE, "CRTC_H");
  if (!(p_conn_crtc_ && p_crtc_mode_ && p_crtc_active_ && p_fb_ && p_plane_crtc_ && p_sx_ && p_sy_ && p_sw_ && p_sh_ && p_dx_ && p_dy_ && p_dw_ && p_dh_)) {
    fprintf(stderr, "[drm] atomic 속성 id 를 찾지 못함\n");
    return false;
  }
  if (drmModeCreatePropertyBlob(fd_, mode_, sizeof(drmModeModeInfo), &mode_blob_) != 0) { fprintf(stderr, "[drm] 모드 blob 생성 실패\n"); return false; }
  return true;
}

bool DrmOut::AllocFbs() {
  hor_ = (mode_w_ + 15) & ~15;   // RGA/VOP 정렬
  ver_ = (mode_h_ + 15) & ~15;
  for (auto& f : fbs_) {
    if (!f.buf.Alloc((size_t)hor_ * ver_ * 3 / 2)) return false;
    if (drmPrimeFDToHandle(fd_, f.buf.fd, &f.handle) != 0) { perror("[drm] drmPrimeFDToHandle"); return false; }
    uint32_t handles[4] = {f.handle, f.handle, 0, 0};
    uint32_t pitches[4] = {(uint32_t)hor_, (uint32_t)hor_, 0, 0};
    uint32_t offsets[4] = {0, (uint32_t)hor_ * ver_, 0, 0};
    if (drmModeAddFB2(fd_, mode_w_ & ~1, mode_h_ & ~1, DRM_FORMAT_NV12, handles, pitches, offsets, &f.id, 0) != 0) {
      perror("[drm] drmModeAddFB2(NV12)");
      return false;
    }
  }
  return true;
}

int DrmOut::Open(const std::string& output) {
  Close();
  int rc = 1;
  if (!Lease(output, rc)) {
    Close();
    if (rc != 3) return rc;   // 3 = X 서버가 없음 -> lease 대신 직접 열기
    if (!OpenDirect(output, rc)) { Close(); return rc; }
  }
  if (!Setup() || !AllocFbs()) { Close(); return 1; }
  printf("[drm] %s 를 DRM 으로 직접 출력합니다 (lease fd=%d, plane %u, %dx%d)\n", output.c_str(), fd_, plane_, mode_w_, mode_h_);
  return 0;
}

bool DrmOut::Present(const FillFn& fill) {
  if (fd_ < 0) return false;
  Fb& fb = fbs_[cur_];
  fb.buf.BeginCpuWrite();   // 캐시 동기화(RGA 가 쓴 내용을 화면 쪽에서 읽도록)
  const bool ok = fill(fb.buf.fd, mode_w_ & ~1, mode_h_ & ~1, hor_, ver_);
  fb.buf.EndCpuWrite();
  if (!ok) return false;

  drmModeAtomicReq* req = drmModeAtomicAlloc();
  if (!req) return false;
  uint32_t flags = 0;
  if (first_) {   // 첫 프레임: 모드 설정 + 출력 켜기
    drmModeAtomicAddProperty(req, conn_, p_conn_crtc_, crtc_);
    drmModeAtomicAddProperty(req, crtc_, p_crtc_mode_, mode_blob_);
    drmModeAtomicAddProperty(req, crtc_, p_crtc_active_, 1);
    flags = DRM_MODE_ATOMIC_ALLOW_MODESET;
  }
  drmModeAtomicAddProperty(req, plane_, p_plane_crtc_, crtc_);
  drmModeAtomicAddProperty(req, plane_, p_fb_, fb.id);
  drmModeAtomicAddProperty(req, plane_, p_sx_, 0);
  drmModeAtomicAddProperty(req, plane_, p_sy_, 0);
  drmModeAtomicAddProperty(req, plane_, p_sw_, (uint64_t)(mode_w_ & ~1) << 16);
  drmModeAtomicAddProperty(req, plane_, p_sh_, (uint64_t)(mode_h_ & ~1) << 16);
  drmModeAtomicAddProperty(req, plane_, p_dx_, 0);
  drmModeAtomicAddProperty(req, plane_, p_dy_, 0);
  drmModeAtomicAddProperty(req, plane_, p_dw_, mode_w_ & ~1);
  drmModeAtomicAddProperty(req, plane_, p_dh_, mode_h_ & ~1);
  const int r = drmModeAtomicCommit(fd_, req, flags, nullptr);   // 비동기 플래그 없이: 다음 vblank 에 반영된 뒤 반환 -> 다른 버퍼는 이미 화면에서 내려감
  drmModeAtomicFree(req);
  if (r != 0) {
    fprintf(stderr, "[drm] atomic commit 실패: %s\n", strerror(errno));
    return false;
  }
  first_ = false;
  cur_ ^= 1;
  return true;
}

void DrmOut::Close() {
  if (fd_ >= 0) {
    for (auto& f : fbs_) {
      if (f.id) drmModeRmFB(fd_, f.id);
      f.id = 0;
      f.handle = 0;
    }
    if (mode_blob_) drmModeDestroyPropertyBlob(fd_, mode_blob_);
    close(fd_);   // lease 종료
    fd_ = -1;
  }
  for (auto& f : fbs_) f.buf.Free();
  mode_blob_ = 0;
  plane_ = 0;
  first_ = true;
  cur_ = 0;
  direct_ = false;
  conn_ = crtc_ = 0;
  crtc_idx_ = 0;
  if (xcb_) { xcb_disconnect(xcb_); xcb_ = nullptr; }
}
#endif
