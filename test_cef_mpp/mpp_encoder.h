#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <rockchip/rk_mpi.h>

#include "dma_heap_buf.h"
#include "video_source.h"

// BGRA -> (RGA) NV12 -> (MPP) H.264
// NV12 버퍼 3개를 돌려 써서, 변환(다른 스레드/콜백)과 인코딩이 서로 기다리지 않게 함.
class MppH264Encoder {
 public:
  using PacketCb = std::function<void(const uint8_t* data, size_t len)>;

  ~MppH264Encoder() { Deinit(); }
  bool Init(int width, int height, int fps, int bitrate_bps);

  // 입력 1) CPU 버퍼(OnPaint): BGRA 1920x1080 연속 메모리
  bool Convert(const uint8_t* bgra);
  // 입력 2) dmabuf(OnAcceleratedPaint): fd, 1행 바이트 수(stride), 포맷(BGRA 또는 RGBA)
  bool ConvertDmaBuf(int fd, int stride_bytes, bool bgra);

  // 입력 3) 영상 합성: UI(BGRA, 프리멀티플라이드 알파)를 dmabuf 로 올려 두고,
  //         매 프레임 영상(NV12 dmabuf)을 캔버스에 비율 맞춰 넣은 뒤 UI 를 알파 합성 -> NV12
  bool UploadUi(const uint8_t* bgra);   // UI 가 바뀌었을 때만 호출 (CPU 복사 1회)
  bool Compose(const VideoFrameRef* video, int tx, int ty, int tw, int th);   // video=nullptr 이면 UI 만
  double TakeComposeMs();   // 직전 호출 이후 Compose 평균 소요(ms)

  // 가장 최근에 변환된 프레임 인코딩. 아직 변환된 프레임이 없으면 false
  bool Encode(const PacketCb& cb);
  void Reset();     // 변환된 프레임 폐기 (로딩 완료 시점에 호출)
  void Deinit();

  // 미리보기(--preview): 직전 Encode() 가 사용한 프레임(NV12)을 RGA 로 축소+BGRX 변환.
  // EncodeLoop 스레드에서 Encode() 직후 동기 호출 전제(그 버퍼가 아직 재사용되지 않음).
  bool ExportPreviewBgrx(uint8_t* out, int out_w, int out_h);

 private:
  int AcquireWrite();   // 읽는 중/최신이 아닌 버퍼 index

  int w_ = 0, h_ = 0, hor_ = 0, ver_ = 0;
  MppCtx ctx_ = nullptr;
  MppApi* mpi_ = nullptr;
  MppEncCfg cfg_ = nullptr;
  MppBufferGroup grp_ = nullptr;
  MppBuffer bufs_[3] = {nullptr, nullptr, nullptr};
  DmaHeapBuf canvas_;   // 합성용 BGRA
  DmaHeapBuf ui_;       // HTML UI (BGRA)
  bool ui_valid_ = false;
  int canvas_key_[4] = {-1, -1, -1, -1};   // 캔버스에 마지막으로 채운 영상 영역 (바뀔 때만 검정 채움)
  bool composite_ok_ = true;               // imcomposite(UI+캔버스 -> NV12) 지원 여부 (실패 시 4단계 방식)
  double compose_ms_sum_ = 0;
  int compose_n_ = 0;
  std::mutex mu_;
  int ready_ = -1;      // 최신 변환 완료 버퍼
  int encoding_ = -1;   // 인코딩 중인 버퍼
  int last_idx_ = -1;   // Encode() 가 마지막으로 사용한 버퍼 (미리보기용)
};
