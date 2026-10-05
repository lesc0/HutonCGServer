// 문자발생기(CG): 저작(--edit) / 실행(--run)
//   ./cg-streamer --edit  [--project=project.json]
//   ./cg-streamer --run   [--project=project.json] [--udp=host:port | --out=URL또는파일.ts] [--seconds=N]
//   실험: cmake -DENABLE_ACCEL_PAINT=ON 빌드 후 --accel (OnAcceleratedPaint dmabuf, GPU 경로)
//         --gpu : OnPaint 경로 그대로, GPU 합성만 켬 (disable-gpu 생략, 예: --cef:use-angle=gles-egl)
//   실험: cmake -DENABLE_EXTERNAL_BEGIN_FRAME=ON 빌드(CG_EBF) 시, CEF 내부 60Hz 타이머 대신
//         EncodeLoop 틱(kFps)이 매번 SendExternalBeginFrame 으로 직접 그리기를 요청한다.
//         기본 빌드(OFF)는 기존처럼 windowless_frame_rate 기반 CEF 자체 타이머를 그대로 사용.
//   --http=PORT : HTTP 제어 포트 (기본 5555)  --bind=ADDR (기본 127.0.0.1)  --token=문자열 (Authorization: Bearer)
//   --jsprof : 렌더러(JS) 프레임 시간(rAF 간격, frame()/render() 소요, GC 추정)을 1초마다 [jstat] 로 기록 (진단용)
//   --autoplay : 로딩 후 Run Setting 시작 페이지부터 자동 재생 (기본은 출력을 비워 두고 명령 대기)
//   --view : 전체화면 창으로 재생 (DISPLAY 필요, 인코딩 없음)
//   --preview : 인코딩(UI+영상 합성)은 그대로 하면서, 그 결과를 별도 X11 창(DISPLAY 필요)에도 표시
//         CG_DRM_OUT=1 ./build.sh 로 빌드(CG_DRM_OUT)하면 X 창 대신 송출 모니터(output_display, 기본 HDMI-2)에 DRM/KMS 로 직접 출력한다
//         (X 의 RandR lease 로 그 모니터 출력을 빌려 옴 -> X 에서는 꺼짐, 입력 없음. 코드는 drm_out.cpp)
//   --no-encode : 렌더링만 (인코딩/전송 없음, CEF 자체 부하 측정용)
//   --no-sync : UI 지터 버퍼 끔 (항상 최신 그림 — 지연 0 이지만 CEF 박자와 어긋나 0/2프레임씩 끊김)
//   --paint-fps=N : CEF 렌더링 fps (기본 60). 인코딩/출력은 60fps 유지(직전 프레임 반복)
//   --dump-onpaint=path.mp4 [--dump-seconds=N, 기본 8] : 진단용. OnPaint 원본 BGRA 를
//         RGA/MPP/UDP 전혀 안 거치고 소프트웨어 H.264(libx264)로 그대로 mp4 기록.
//         PTS 는 실제 도착 시각(ms) 그대로(VFR) - CEF 자체가 고르게 그리는지 영상에 그대로 드러남.
//         추가 크로미움 스위치: --cef:이름[=값]  (예: --cef:use-angle=gles)
//   --setup=cgsetup.cfg : 송출 설정 파일 (기본 ./cgsetup.cfg, 없으면 기본값 사용)
//         output=1(HDMI만) 2(UDP만, 기본) 3(HDMI+UDP) / udp_ip=... / udp_port=... / fps=... (기본 60)
//         커맨드라인의 --udp=/--out=/--view/--preview/--paint-fps= 는 이 파일보다 항상 우선
//   출력: H.264(MPP) -> MPEG-TS(ffmpeg muxer) -> UDP (기본 udp://127.0.0.1:1234)
//   --run 은 동시에 하나만: 두 번째 실행은 즉시 종료됨 (/tmp/cg-streamer.run.lock)
// 실행 중 제어(UDP 127.0.0.1:5555):  echo next | nc -u -w0 127.0.0.1 5555
//   next | prev | goto N | quit
// 영상(페이지 JSON 의 "video"): 네이티브 MPP 디코딩 -> RGA 로 HTML UI(투명 배경)와 합성 -> 인코딩
//   player.html 이 cefQuery 로 요청: video:play:<경로> | video:stop | video:rect:x,y,w,h
//   "video": "hdmirx" (또는 "/dev/videoN") 이면 HDMI 입력(rk_hdmirx)을 라이브 소스로 합성
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <chrono>
#include <cstdio>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "include/base/cef_callback.h"
#include "include/cef_app.h"
#include "include/cef_client.h"
#include "include/views/cef_browser_view.h"
#include "include/views/cef_window.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_message_router.h"
#include "mpp_encoder.h"
#include "hdmirx_source.h"
#include "video_source.h"
#include "ts_muxer.h"
#include "audio_mixer.h"
#include "rt.h"
#include "cef_dumper.h"
#include "log_writer.h"
#include "drm_out.h"   // CG_DRM_OUT 빌드에서만 내용이 있음

// X11 은 CEF 헤더 뒤에 포함(Success/None 등 매크로가 CEF의 동명 심볼과 충돌).
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xrandr.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#undef Success

extern char** environ;

static constexpr int kW = 1920, kH = 1080;
static int g_fps = 60;   // cgsetup.cfg: fps=... (인코더 rc/EncodeLoop 틱/기본 페인트 fps 공통)
static constexpr int kBitrate = 8 * 1000 * 1000;
static constexpr int kCtlPort = 5555;

// ---------- 모드 구분 ----------
// AppMode   : 프로세스 전체의 동작 모드 (--edit 저작 / --run 실행)
// PaintMode : --run 모드에서 화면을 어떻게 캡처하는지 (소프트웨어 OnPaint / 실험용 dmabuf 가속)
enum class AppMode { kEdit, kRun };
enum class PaintMode { kSoftware, kAccel };

static AppMode g_app_mode = AppMode::kEdit;
static PaintMode g_paint_mode = PaintMode::kSoftware;
static bool g_gpu_composite = false;             // --gpu: kSoftware 에서도 GPU 합성 유지
static bool g_view = false;                      // --view: OSR 대신 전체화면 창으로 재생 (HDMI 확인용, 인코딩 없음)
static bool g_preview = false;                   // --preview: 인코딩(UI+영상 합성) 결과를 그대로 로컬 X11 창에도 표시
static bool g_encode = true;                     // --no-encode: 렌더링만 (RGA/MPP/TS 없음, CEF 부하 측정용)
static int g_paint_fps = g_fps;                  // --paint-fps=N: CEF 렌더링 fps (기본은 g_fps 와 동일)
static bool g_sync = true;                       // UI 지터 버퍼 사용 (--no-sync: 최신 그림만)

static std::string g_project = "project.json";
static std::string g_out = "udp://127.0.0.1:1234?pkt_size=1316";
static std::string g_exe, g_webdir;
// 인코더가 새 프레임을 만들 때마다 증가시켜 미리보기를 깨운다(타이머로 그리면 16ms=62.5Hz 처럼 모니터 60Hz 와 어긋나 초당 2~3번 프레임이 튄다)
static std::mutex g_pv_mu;
static std::condition_variable g_pv_cv;
static uint64_t g_pv_seq = 0;
static std::atomic<bool> g_preview_reset{false};          // 로컬 화면 해상도가 바뀌어 미리보기 창을 다시 만들어야 함
static std::atomic<int> g_preview_w{0}, g_preview_h{0};   // 현재 미리보기 창 크기
static std::atomic<int> g_preview_x{0}, g_preview_y{0};   // 현재 미리보기 창 위치(모니터 두 대일 때 송출 모니터 위치)
static std::string g_output_display = "HDMI-2";           // 송출 화면을 보여 줄 모니터(cgsetup.cfg output_display). 에디터는 editor_display(기본 HDMI-1)
static std::string g_page = "player.html";   // --page=이름.html : 진단/테스트용, bin/web/ 기준 다른 페이지 로드

// cgsetup.cfg: output=1(HDMI만) 2(UDP만, 기본) 3(HDMI+UDP) / udp_ip / udp_port
// 입력(HDMI RX)은 자동 감지이므로 설정 대상 아님. --setup= 로 경로 변경 가능, 커맨드라인 인자가 항상 우선.
static std::string g_setup_cfg = "cgsetup.cfg";
static int g_output_type = 2;
static std::string g_audio_out = "auto";   // 로컬 음성 출력: auto(output=3 일 때 연결된 HDMI 소리 카드) / off / ALSA 장치명
static std::string g_udp_ip = "127.0.0.1";
static int g_udp_port = 1234;

// 진단용: --dump-onpaint=path.mp4 [--dump-seconds=N] - OnPaint 원본을 RGA/MPP/UDP 없이 그대로 mp4 로 기록
// 지금은 안 씀 (끊김 원인이 CEF 자체 렌더링임을 확인하는 데 썼음) - 코드는 남겨두고 꺼둠
#if 0
static std::string g_dump_path;
static int g_dump_seconds = 8;
static CefDumper g_dumper;
static std::chrono::steady_clock::time_point g_dump_t_end;
#endif

// HTTP 제어 (--http=PORT --bind=ADDR --token=TOKEN). 기본은 loopback 전용, 토큰 없음.
static int g_http_port = kCtlPort;               // TCP 와 UDP 포트는 별개라 같은 번호를 써도 충돌하지 않는다
static std::string g_http_bind = "127.0.0.1";
static std::string g_http_token;
static bool g_autoplay = false;                  // --autoplay: 로딩 후 Run Setting 시작 페이지부터 자동 재생
static bool g_jsprof = false;                    // --jsprof: 렌더러(JS) 프레임 시간을 1초마다 [jstat] 로 기록 (진단용)
static std::string PlayerQuery() {              // player.html URL 의 쿼리스트링
  std::string q;
  if (g_autoplay) q += "?autoplay=1";
  if (g_jsprof) q += q.empty() ? "?jsprof=1" : "&jsprof=1";
  return q;
}
static std::mutex g_state_mu;                    // player 가 cefQuery('state:...') 로 올려 주는 상태 (GET /status)
static std::string g_state = "{\"ready\":false}";
static std::atomic<bool> g_ready{false};   // player.html 로딩 완료 신호
static std::atomic<bool> g_quit{false};
static std::atomic<uint64_t> g_paints{0};
// OnPaint 도착 간격 진단(1초 단위로 [estat] 에 출력 후 초기화): 가장 큰 간격(ms)과 25ms(60fps 의 1.5프레임) 넘게 늦은 횟수
static std::atomic<uint32_t> g_gap_max_us{0}, g_gap_late{0}, g_gap_big{0};
// OnPaint 한 번에 걸리는 시간(복사·락 포함) 진단: CEF 는 OnPaint 가 끝나야 다음 프레임을 진행하므로 오래 걸리면 프레임을 놓친다
static std::atomic<uint64_t> g_op_sum_us{0}, g_op_n{0};
static std::atomic<uint32_t> g_op_max_us{0};
static double OnPaintAvgMs() {   // 마지막 호출 이후 평균(ms). 호출하면 초기화
  const uint64_t n = g_op_n.exchange(0), sum = g_op_sum_us.exchange(0);
  return n ? sum / 1000.0 / n : 0.0;
}
struct OnPaintTimer {
  std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
  ~OnPaintTimer() {
    const uint32_t us = (uint32_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
    g_op_sum_us += us; g_op_n++;
    uint32_t m = g_op_max_us.load();
    while (us > m && !g_op_max_us.compare_exchange_weak(m, us)) {}
  }
};
static pid_t g_child = 0;
static CefRefPtr<CefMessageRouterBrowserSide> g_router;

// UI 지터 버퍼: CEF 는 자체 60Hz 로 그리고 인코더는 별도 60Hz 타이머로 돈다. 두 박자가 어긋나면
// "같은 그림 2번 / 1장 건너뜀"이 생겨 티커 등이 끊겨 보이므로, 그림을 5장 쌓아 두고(지연 약 83ms)
// 매 틱 1장씩 꺼내 도착 간격의 흔들림을 흡수한다.
struct UiFrame {
  std::vector<uint8_t> px;                          // BGRA (프리멀티플라이드 알파)
  uint64_t seq = 0;                                 // 0 = 없음
  std::chrono::steady_clock::time_point at;         // OnPaint 도착 시각
};
struct FrameStore {
  std::mutex m;
  std::deque<UiFrame> q;                            // OnPaint -> EncodeLoop
  std::vector<std::vector<uint8_t>> pool;           // 버퍼 재사용 (매번 8MB 할당 방지)
  uint64_t seq = 0;
  uint64_t dropped = 0;                             // 큐가 넘쳐 버린 그림 수
  std::chrono::steady_clock::time_point last_at;    // 마지막 OnPaint 도착 시각
};
static FrameStore g_store;
static constexpr size_t kUiPrime = 5;               // 이만큼 쌓이면 출력 시작 = 목표 버퍼 수위 (지연 약 5프레임)
static constexpr size_t kUiMax = 8;                 // 순간적으로 몰려도 여기까지는 보관, 넘으면 가장 오래된 그림을 버림
// kUiPrime 3->5: CEF 가 부하로 프레임을 한 번에 2~3장 연속으로 건너뛸 때(버스트) under 가 한꺼번에
// 뛰면서 자막/티커가 눈에 띄게 끊기는 문제 완화. 지연이 ~50ms -> ~83ms 로 늘지만 방송 그래픽 용도엔 무해.

static MppH264Encoder g_encoder;                 // OnPaint/OnAcceleratedPaint 공용
static std::atomic<bool> g_enc_ok{false};        // 인코더 Init 완료
static std::vector<std::string> g_extra;         // --cef: 로 받은 스위치
static std::atomic<uint64_t> g_accel_fail{0};

// 영상 (HTML <video> 대신 네이티브 MPP 디코딩, RGA 로 UI 와 합성)
static VideoSource g_video;
static AudioMixer g_audio;                       // 영상/HDMI 음성 -> AAC -> TS (EncodeLoop 이 시작/종료)
static HdmiRxSource g_hdmi;                      // HDMI 입력 라이브 소스
static std::mutex g_video_mu;                    // g_video_path / g_video_rect
static std::string g_video_path;
static int g_video_rect[4] = {0, 0, kW, kH};    // 영상을 맞춰 넣을 영역 (비율 유지)

static void SendUdp(const std::string& s) {
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(kCtlPort);
  inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
  sendto(fd, s.data(), s.size(), 0, (sockaddr*)&a, sizeof a);
  close(fd);
}

static bool ReadFile(const std::string& path, std::string& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::stringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}

// cgsetup.cfg 로드: "키=값" 줄 단위, '#' 이후는 주석. 파일이 없으면 기본값 그대로 조용히 진행.
static void LoadSetupCfg(const std::string& path) {
  std::ifstream f(path);
  if (!f) return;
  auto trim = [](std::string s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
  };
  std::string line;
  while (std::getline(f, line)) {
    size_t h = line.find('#');
    if (h != std::string::npos) line.resize(h);
    size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
    if (k == "output") g_output_type = atoi(v.c_str());
    else if (k == "udp_ip") g_udp_ip = v;
    else if (k == "udp_port") g_udp_port = atoi(v.c_str());
    else if (k == "audio_out") g_audio_out = v;
    else if (k == "output_display") g_output_display = v;
    else if (k == "realtime") g_rt_enabled = (v == "on" || v == "1" || v == "true");
    else if (k == "fps") g_fps = std::max(1, atoi(v.c_str()));
  }
}

// ---------- 클라이언트 ----------
class Client : public CefClient,
               public CefRenderHandler,
               public CefLifeSpanHandler,
               public CefMessageRouterBrowserSide::Handler {
 public:
  explicit Client(bool osr) : osr_(osr) {}

  CefRefPtr<CefRenderHandler> GetRenderHandler() override { return osr_ ? this : nullptr; }
  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }

  bool OnProcessMessageReceived(CefRefPtr<CefBrowser> b, CefRefPtr<CefFrame> f,
                                CefProcessId src, CefRefPtr<CefProcessMessage> m) override {
    return g_router->OnProcessMessageReceived(b, f, src, m);
  }

  // ===== [RUN MODE - PaintMode::kSoftware] =====
  // CEF가 CPU 메모리(BGRA)로 렌더링한 화면을 받아 프레임 버퍼에 복사해 둔다.
  // 실제 RGA 변환은 EncodeLoop() 쪽에서 이 버퍼를 읽어 수행한다.
  void GetViewRect(CefRefPtr<CefBrowser>, CefRect& rect) override { rect = CefRect(0, 0, kW, kH); }
  void OnPaint(CefRefPtr<CefBrowser>, PaintElementType type, const RectList&,
               const void* buffer, int w, int h) override {
    if (g_paint_mode != PaintMode::kSoftware) return;
    if (type != PET_VIEW || w != kW || h != kH) return;
    OnPaintTimer op_timer;   // 이 함수가 끝날 때 소요 시간을 기록
    {   // 도착 간격 진단
      static auto last = std::chrono::steady_clock::time_point{};
      const auto now = std::chrono::steady_clock::now();
      if (last != std::chrono::steady_clock::time_point{}) {
        const uint32_t us = (uint32_t)std::chrono::duration_cast<std::chrono::microseconds>(now - last).count();
        uint32_t m = g_gap_max_us.load();
        while (us > m && !g_gap_max_us.compare_exchange_weak(m, us)) {}
        if (us > 25000) g_gap_late++;
        if (us > 100000) g_gap_big++;
      }
      last = now;
    }
#if 0   // 진단용 OnPaint 덤프 (지금은 꺼둠: --no-encode 유휴 상태에서만 측정되어 판단 근거로 부적절했음)
    if (g_dumper.Active()) {
      if (std::chrono::steady_clock::now() < g_dump_t_end) g_dumper.PushFrame(buffer);
      else g_dumper.Close();
    }
#endif
    if (!g_encode) { g_paints++; return; }   // --no-encode: 복사도 생략
    UiFrame f;
    f.at = std::chrono::steady_clock::now();
    {
      std::lock_guard<std::mutex> lk(g_store.m);
      if (!g_store.pool.empty()) { f.px = std::move(g_store.pool.back()); g_store.pool.pop_back(); }
    }
    f.px.resize((size_t)w * h * 4);
    memcpy(f.px.data(), buffer, (size_t)w * h * 4);  // BGRA
    std::lock_guard<std::mutex> lk(g_store.m);
    f.seq = ++g_store.seq;
    g_store.last_at = f.at;
    g_store.q.push_back(std::move(f));
    if (g_store.q.size() > kUiMax) {
      g_store.pool.push_back(std::move(g_store.q.front().px));
      g_store.q.pop_front();
      g_store.dropped++;
    }
    g_paints++;
  }

#ifdef CG_ACCEL
  // ===== [RUN MODE - PaintMode::kAccel] (실험) =====
  // GPU가 합성한 화면을 dmabuf로 직접 받아 RGA에서 바로 NV12로 변환한다(제로카피).
  // 필드명(plane_count, planes[].fd/stride, modifier, format)은 사용 중인 CEF 헤더
  // (include/internal/cef_types_linux.h)와 대조 필요.
  void OnAcceleratedPaint(CefRefPtr<CefBrowser>, PaintElementType type, const RectList&,
                          const CefAcceleratedPaintInfo& info) override {
    if (g_paint_mode != PaintMode::kAccel) return;
    if (type != PET_VIEW) return;
    g_paints++;

    static int logged = 0;
    if (logged < 5) {
      logged++;
      printf("[accel] planes=%d fd0=%d stride0=%u offset0=%llu modifier=0x%llx format=%d\n",
             (int)info.plane_count, info.planes[0].fd, (unsigned)info.planes[0].stride,
             (unsigned long long)info.planes[0].offset, (unsigned long long)info.modifier,
             (int)info.format);
    }
    if (!g_enc_ok || info.plane_count < 1) return;

    // 리니어(0) 또는 미지정(INVALID)만 RGA 로 읽을 수 있음. 압축(AFBC 등)은 건너뜀.
    const uint64_t kLinear = 0, kInvalid = 0x00ffffffffffffffULL;
    if (info.modifier != kLinear && info.modifier != kInvalid) {
      static bool warned = false;
      if (!warned) {
        warned = true;
        fprintf(stderr, "[accel] non-linear modifier 0x%llx: 변환 불가, 프레임 건너뜀\n",
                (unsigned long long)info.modifier);
      }
      g_accel_fail++;
      return;
    }
    // 콜백이 끝나면 CEF 가 버퍼를 회수하므로, 여기서 동기적으로 NV12 로 변환해 둔다.
    const bool bgra = (info.format == CEF_COLOR_TYPE_BGRA_8888);
    if (!g_encoder.ConvertDmaBuf(info.planes[0].fd, (int)info.planes[0].stride, bgra))
      g_accel_fail++;
  }
#endif

  // --- 수명 ---
  void OnAfterCreated(CefRefPtr<CefBrowser> b) override { browser_ = b; }
  void OnBeforeClose(CefRefPtr<CefBrowser> b) override {
    g_router->OnBeforeClose(b);
    browser_ = nullptr;
    CefQuitMessageLoop();
  }
  void Close() { if (browser_) browser_->GetHost()->CloseBrowser(true); }
  void Exec(std::string js) {
    if (browser_) browser_->GetMainFrame()->ExecuteJavaScript(js, "", 0);
  }
#ifdef CG_EBF
  // browser_는 UI 스레드에서만 접근한다. UI가 늦으면 요청을 누적하지 않는다.
  void RequestBeginFrame() {
    if (begin_frame_pending_.exchange(true)) return;
    if (!CefPostTask(TID_UI, base::BindOnce(&Client::BeginFrame, CefRefPtr<Client>(this))))
      begin_frame_pending_ = false;
  }
  void BeginFrame() {
    begin_frame_pending_ = false;
    if (!g_quit && browser_ && osr_) browser_->GetHost()->SendExternalBeginFrame();
  }
  std::atomic<bool> begin_frame_pending_{false};
#endif

  // --- JS -> 네이티브 (cefQuery) ---
  bool OnQuery(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, int64_t, const CefString& request,
               bool, CefRefPtr<Callback> cb) override {
    const std::string r = request.ToString();

    if (r.rfind("prof:", 0) == 0) {   // --jsprof: player 의 프레임 시간 측정값
      printf("[jstat] %s\n", r.c_str() + 5);
      cb->Success("ok");
      return true;
    }
    if (r.rfind("save:", 0) == 0) {
      std::ofstream f(g_project, std::ios::binary | std::ios::trunc);
      if (!f) { cb->Failure(1, "cannot write " + g_project); return true; }
      f << r.substr(5);
      cb->Success("ok");
      return true;
    }
    if (r == "base") {            // 프로젝트 폴더 URL: player 가 이미지 상대경로를 이 기준으로 해석
      cb->Success("file://" + std::filesystem::path(g_project).parent_path().string() + "/");
      return true;
    }
    if (r.rfind("state:", 0) == 0) {   // player 상태 보고 (JSON)
      std::lock_guard<std::mutex> lk(g_state_mu);
      g_state = r.substr(6);
      cb->Success("ok");
      return true;
    }
    if (r == "load") {
      std::string s;
      if (!ReadFile(g_project, s)) cb->Failure(2, "no project file");
      else cb->Success(s);
      return true;
    }
    if (r == "ready") {           // player 로딩 완료 -> 새 프레임부터 인코딩 시작
      {
        std::lock_guard<std::mutex> lk(g_store.m);   // 로딩 중 그림 폐기
        for (auto& f : g_store.q) g_store.pool.push_back(std::move(f.px));
        g_store.q.clear();
      }
      g_encoder.Reset();
      if (browser_) browser_->GetHost()->Invalidate(PET_VIEW);
      g_ready = true;
      printf("[cg] loaded, encoding start\n");
      cb->Success("ok");
      return true;
    }
    if (r == "run") {             // 편집기에서 실행 프로세스 기동
      if (g_child > 0 && waitpid(g_child, nullptr, WNOHANG) == 0) {
        cb->Success("already running");
        return true;
      }
      std::string a1 = "--run", a2 = "--project=" + g_project, a3 = "--out=" + g_out;  // UDP URL 그대로 전달
      char* args[] = {(char*)g_exe.c_str(), a1.data(), a2.data(), a3.data(), nullptr};
      if (posix_spawn(&g_child, g_exe.c_str(), nullptr, nullptr, args, environ) != 0)
        cb->Failure(3, "spawn failed");
      else
        cb->Success("started");
      return true;
    }
    if (r.rfind("video:", 0) == 0) {   // 영상 제어 (페이지 전환 시 player.html 이 호출)
      std::lock_guard<std::mutex> lk(g_video_mu);
      if (r.rfind("video:play:", 0) == 0) {
        namespace fs = std::filesystem;
        const std::string arg = r.substr(11);
        if (arg == "hdmirx" || arg.rfind("/dev/video", 0) == 0) {          // HDMI 입력
          const std::string dev = arg == "hdmirx" ? "/dev/video20" : arg;
          if (dev != g_video_path || !g_hdmi.Active()) {
            g_video.Stop();
            g_video_path = dev;
            g_hdmi.Start(dev);
          }
        } else {
          fs::path p = arg;
          if (p.is_relative()) p = fs::path(g_project).parent_path() / p;   // 프로젝트 파일 기준
          if (p.string() != g_video_path || !g_video.Active()) {             // 같은 영상이면 계속 재생
            g_hdmi.Stop();
            g_video_path = p.string();
            g_video.Play(g_video_path, true);
          }
        }
      } else if (r == "video:stop") {
        g_video.Stop();
        g_hdmi.Stop();
        g_video_path.clear();
        g_video_rect[0] = 0, g_video_rect[1] = 0, g_video_rect[2] = kW, g_video_rect[3] = kH;
      } else if (r.rfind("video:rect:", 0) == 0) {
        int v[4];
        if (sscanf(r.c_str() + 11, "%d,%d,%d,%d", &v[0], &v[1], &v[2], &v[3]) == 4 && v[2] > 0 && v[3] > 0)
          std::copy(v, v + 4, g_video_rect);
      }
      cb->Success("ok");
      return true;
    }
    if (r.rfind("cmd:", 0) == 0) {  // next / prev / quit
      SendUdp(r.substr(4));
      cb->Success("ok");
      return true;
    }
    return false;
  }

 private:
  bool osr_;
  CefRefPtr<CefBrowser> browser_;
  IMPLEMENT_REFCOUNTING(Client);
};

// ---------- App (렌더러 쪽 cefQuery 주입) ----------
class App : public CefApp, public CefRenderProcessHandler {
 public:
  CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override { return this; }

  void OnBeforeCommandLineProcessing(const CefString&, CefRefPtr<CefCommandLine> cl) override {
    cl->AppendSwitch("allow-file-access-from-files");
    if (g_app_mode == AppMode::kRun && g_paint_mode == PaintMode::kSoftware && !g_gpu_composite &&
        !g_view) {  // 소프트웨어 OnPaint 경로 (kAccel 이면 GPU 유지)
      cl->AppendSwitch("disable-gpu");
      cl->AppendSwitch("disable-gpu-compositing");
    }
    for (const auto& e : g_extra) {   // --cef:이름[=값]
      auto eq = e.find('=');
      if (eq == std::string::npos) cl->AppendSwitch(e);
      else cl->AppendSwitchWithValue(e.substr(0, eq), e.substr(eq + 1));
    }
  }
  void OnWebKitInitialized() override {
    renderer_router_ = CefMessageRouterRendererSide::Create(CefMessageRouterConfig());
  }
  void OnContextCreated(CefRefPtr<CefBrowser> b, CefRefPtr<CefFrame> f,
                        CefRefPtr<CefV8Context> c) override {
    renderer_router_->OnContextCreated(b, f, c);
  }
  void OnContextReleased(CefRefPtr<CefBrowser> b, CefRefPtr<CefFrame> f,
                         CefRefPtr<CefV8Context> c) override {
    renderer_router_->OnContextReleased(b, f, c);
  }
  bool OnProcessMessageReceived(CefRefPtr<CefBrowser> b, CefRefPtr<CefFrame> f, CefProcessId src,
                                CefRefPtr<CefProcessMessage> m) override {
    return renderer_router_->OnProcessMessageReceived(b, f, src, m);
  }

 private:
  CefRefPtr<CefMessageRouterRendererSide> renderer_router_;
  IMPLEMENT_REFCOUNTING(App);
};

// ---------- 저작 창 (Views) ----------
class WinDelegate : public CefWindowDelegate {
 public:
  explicit WinDelegate(CefRefPtr<CefBrowserView> v, bool fullscreen = false)
      : view_(v), fullscreen_(fullscreen) {}
  void OnWindowCreated(CefRefPtr<CefWindow> w) override {
    w->AddChildView(view_);
    if (!fullscreen_) w->Maximize();
    w->Show();
    if (fullscreen_) w->SetFullscreen(true);   // Show() 이후에 적용해야 반영됨
    view_->RequestFocus();
  }
  void OnWindowDestroyed(CefRefPtr<CefWindow>) override { view_ = nullptr; }
  bool CanClose(CefRefPtr<CefWindow>) override {
    auto b = view_ ? view_->GetBrowser() : nullptr;
    return b ? b->GetHost()->TryCloseBrowser() : true;
  }
  CefSize GetPreferredSize(CefRefPtr<CefView>) override { return CefSize(1600, 900); }

 private:
  CefRefPtr<CefBrowserView> view_;
  bool fullscreen_;
  IMPLEMENT_REFCOUNTING(WinDelegate);
};

// ---------- 실행 모드 보조 스레드 ----------
static void EncodeLoop(CefRefPtr<Client> client) {
  pthread_setname_np(pthread_self(), "cg-encode");
  SetRealtime("cg-encode", 50);
  if (!g_encoder.Init(kW, kH, g_fps, kBitrate)) { g_quit = true; return; }
  g_enc_ok = true;
  TsMuxer mux;
  if (!mux.Open(g_out, kW, kH, g_fps)) { g_quit = true; return; }
  g_audio.Start(&mux);

  using clk = std::chrono::steady_clock;
  const auto period = std::chrono::nanoseconds(1000000000LL / g_fps);
  // fps 틱: timerfd(CLOCK_MONOTONIC). 첫 만료는 period 뒤, 이후 period 간격의 절대 격자로 만료된다.
  const int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
  if (tfd < 0) { perror("timerfd_create"); g_quit = true; return; }
  {
    itimerspec its{};
    its.it_interval.tv_sec = its.it_value.tv_sec = (time_t)(period.count() / 1000000000LL);
    its.it_interval.tv_nsec = its.it_value.tv_nsec = (long)(period.count() % 1000000000LL);
    if (timerfd_settime(tfd, 0, &its, nullptr) != 0) { perror("timerfd_settime"); close(tfd); g_quit = true; return; }
  }
#ifdef CG_EBF
  auto next = clk::now();
#endif
  uint64_t frames = 0, bytes = 0, last_paints = 0, converted_seq = 0, uploaded_seq = 0, underflows = 0, missed = 0;
  int64_t last_live_ts = 0;     // 직전에 PTS 로 쓴 V4L2 timestamp (같은 프레임을 다시 쓰는 틱 구분용)
  const bool jitter_buf = g_sync && g_paint_fps == g_fps;   // --paint-fps 가 60 미만이면 최신 그림 방식
  UiFrame cur;                  // 현재 출력 중인 UI 그림 (EncodeLoop 소유)
  bool primed = false;
  auto over_since = clk::time_point{};   // 큐가 목표 수위를 넘은 시점 (1초 이상 지속되면 1장 버려 지연 복귀)
  auto t0 = clk::now();
#ifdef CG_EBF
  const auto paint_period = std::chrono::nanoseconds(1000000000LL / g_paint_fps);
  auto next_paint = next;
#endif

  while (!g_quit) {
    uint64_t expirations = 0;     // 마지막 read 이후 만료 횟수. 1 보다 크면 그만큼 틱이 밀린 것
    const ssize_t rn = read(tfd, &expirations, sizeof expirations);
    if (rn != (ssize_t)sizeof expirations) {
      if (rn < 0 && errno == EINTR) continue;
      perror("timerfd read"); break;
    }
    // TODO(정책 4): 밀린 틱(expirations-1)을 따라잡는 처리는 정책서 미결정 3(따라잡기 틱에 쓸 입력) 확정 후 구현.
    // 지금은 한 번만 처리하고 밀린 횟수만 센다(이전 sleep 방식과 같은 동작).
    if (expirations > 1) missed += expirations - 1;
    const int64_t tick_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clk::now().time_since_epoch()).count();
#ifdef CG_EBF
    const auto tick_now = clk::now();
    if (tick_now >= next_paint) {
      client->RequestBeginFrame();
      do { next_paint += paint_period; } while (next_paint <= tick_now);
    }
#endif
    if (!g_ready) {               // 로딩 중 화면은 인코딩하지 않음 (렌더링은 계속 요청해야 ready 가 옴)
      t0 = clk::now();
      continue;
    }

    if (g_paint_mode == PaintMode::kSoftware) {   // 이번 틱에 출력할 UI 그림 선택
      std::lock_guard<std::mutex> lk(g_store.m);
      auto take = [&] {
        if (!cur.px.empty()) g_store.pool.push_back(std::move(cur.px));
        cur = std::move(g_store.q.front());
        g_store.q.pop_front();
      };
      if (!jitter_buf) {
        while (!g_store.q.empty()) take();         // 최신 그림만
      } else {
        // 목표 수위를 확보하거나, 정지 화면의 첫 그림도 목표 지연 후 출력한다
        if (!primed && !g_store.q.empty() &&
            (g_store.q.size() >= kUiPrime || clk::now() - g_store.q.front().at >= period * kUiPrime))
          primed = true;
        if (primed) {
          if (!g_store.q.empty()) take();
          else if (clk::now() - g_store.last_at < period * 2) underflows++;   // 애니메이션 중인데 늦음: 직전 그림 1번 반복
          // 다시 쌓일 때까지 기다리지 않는다(기다리면 반복이 더 늘어남). 늦은 그림 뒤에는 보통
          // 바로 다음 그림이 몰려 와서 수위가 회복된다. 정지 화면이면 새 그림이 없으므로 반복이 정상.
          if (g_store.q.size() > kUiPrime) {      // 수위가 목표보다 높은 상태가 1초 넘게 지속 -> 1장 버려 지연 복귀
            const auto now = clk::now();
            if (over_since == clk::time_point{}) over_since = now;
            else if (now - over_since > std::chrono::seconds(1)) {
              g_store.pool.push_back(std::move(g_store.q.front().px));
              g_store.q.pop_front();
              g_store.dropped++;
              over_since = {};
            }
          } else {
            over_since = {};
          }
        }
      }
    }

    const bool hdmi = g_hdmi.Active();
    // 이번 프레임의 PTS 원천(CLOCK_MONOTONIC ns). 라이브(HDMI RX)는 V4L2 버퍼 timestamp, 그 외(CG 만 / 같은 입력을 다시 쓰는 틱)는 틱 시각.
    int64_t src_ts = tick_ns;
    if (g_paint_mode == PaintMode::kSoftware && (g_video.Active() || hdmi)) {
      // 영상 합성: UI 는 바뀔 때만 dmabuf 로 올리고, 매 프레임 영상+UI 를 RGA 로 합성
      if (cur.seq && cur.seq != uploaded_seq) {
        g_encoder.UploadUi(cur.px.data());
        uploaded_seq = cur.seq;
      }
      int rc[4];
      {
        std::lock_guard<std::mutex> lk(g_video_mu);
        std::copy(g_video_rect, g_video_rect + 4, rc);
      }
      VideoFrameRef vf;   // HDMI 신호가 없으면 영상 자리는 검정 (UI 는 그대로)
      const bool have = hdmi ? g_hdmi.Acquire(vf) : g_video.Acquire(vf);
      if (have && hdmi && vf.ts_ns > 0 && vf.ts_ns != last_live_ts) { src_ts = vf.ts_ns; last_live_ts = vf.ts_ns; }
      const bool ok = g_encoder.Compose(have ? &vf : nullptr, rc[0], rc[1], rc[2], rc[3]);
      if (have) hdmi ? g_hdmi.Release() : g_video.Release();
      converted_seq = 0;                          // 영상이 끝나면 UI 단독 변환을 다시 하도록
      if (!ok) continue;
    } else if (g_paint_mode == PaintMode::kSoftware) {   // OnPaint: 새 그림일 때만 CPU 버퍼 -> RGA
      if (!cur.seq) continue;
      if (cur.seq != converted_seq) {             // 변화 없으면 직전 NV12 버퍼를 그대로 재인코딩
        if (!g_encoder.Convert(cur.px.data())) continue;
        converted_seq = cur.seq;
      }
    }                               // kAccel: 변환은 OnAcceleratedPaint 에서 이미 수행됨
    if (!g_encoder.Encode([&](const uint8_t* d, size_t n) { mux.Write(d, n, src_ts); bytes += n; }))
      continue;                     // 아직 프레임 없음
    frames++;
    { std::lock_guard<std::mutex> lk(g_pv_mu); g_pv_seq++; }
    g_pv_cv.notify_one();

    if (frames % g_fps == 0) {
      double sec = std::chrono::duration<double>(clk::now() - t0).count();
      uint64_t p = g_paints.load();
      size_t ql;
      uint64_t dropped;
      {
        std::lock_guard<std::mutex> lk(g_store.m);
        ql = g_store.q.size();
        dropped = g_store.dropped;
      }
      printf("[estat] enc=%.1ffps paint=%llu/s gap_max=%.0fms late25=%u big100=%u op_avg=%.1fms op_max=%.0fms out=%.2fMbps rga=%.1fms video=%s uiq=%zu under=%llu drop=%llu miss=%llu accel_fail=%llu\n",
             frames / sec, (unsigned long long)(p - last_paints), g_gap_max_us.exchange(0) / 1000.0, g_gap_late.exchange(0),
             g_gap_big.exchange(0), OnPaintAvgMs(), g_op_max_us.exchange(0) / 1000.0, bytes * 8.0 / sec / 1e6, g_encoder.TakeComposeMs(),
             g_hdmi.Active() ? (g_hdmi.HasSignal() ? "hdmirx" : "hdmirx(no-signal)")
                             : g_video.Active() ? "on" : "off", ql, (unsigned long long)underflows,
             (unsigned long long)dropped, (unsigned long long)missed, (unsigned long long)g_accel_fail.load());
      last_paints = p;
      frames = 0; bytes = 0; t0 = clk::now();
    }
  }
  close(tfd);
  g_enc_ok = false;
  g_audio.Stop();
  mux.Close();
}

// --preview: EncodeLoop 이 만든 NV12(UI+영상 합성) 프레임을 RGA 로 축소+BGRX 변환해
// 별도의 평범한 X11 창(override-redirect, GL/EGL 미사용)에 그린다.
// CEF 자체 창(Views/ANGLE)과 무관해 이 보드의 GPU/EGL 드라이버 문제를 우회한다.
//
// MIT-SHM(XShm) + 더블버퍼 사용:
// - 일반 XPutImage 는 매 프레임 전체 화면(1024x600x4=2.4MB)을 X11 소켓으로 복사해 보내야 해서
//   60fps 로 돌리면 그 자체가 병목이 되어 미리보기가 고르지 못하게 끊겨 보인다 -> XShm 으로 공유
//   메모리에 RGA 가 직접 쓰고, X 서버에는 "그 메모리에서 블릿해라"라는 짧은 요청만 보낸다.
// - 버퍼가 1장뿐이면 X 서버가 아직 그 메모리를 화면에 옮기는 중에 RGA 가 같은 메모리를 새 프레임으로
//   덮어써서 화면이 찢어지거나 깨져 보일 수 있다(단일버퍼 티어링) -> 버퍼 2장을 번갈아 쓰고, 각
//   버퍼는 직전 XShmPutImage 의 ShmCompletion 이벤트(=X 서버가 다 읽음)를 받은 뒤에만 재사용한다.
// display.sh 가 기록한 송출 모니터 영역("x y w h", bin/.run/display-output.geom)을 읽는다. 없거나 잘못되면 false(호출한 쪽의 기본값 유지).
static bool ReadOutputGeom(int& x, int& y, int& w, int& h) {
  std::ifstream f(std::filesystem::path(g_exe).parent_path() / ".run" / "display-output.geom");
  int a, b, c, d;
  if (!(f >> a >> b >> c >> d) || c <= 0 || d <= 0 || a < 0 || b < 0) return false;
  x = a; y = b; w = c; h = d;
  return true;
}

// X(RandR)가 지금 알고 있는 출력 name 의 영역(x y w h). 꺼져 있거나 없으면 false.
// display.sh 가 기록한 파일은 연결 변경을 감지(1.5초 이상 안정)한 뒤에야 갱신되므로, X 가 모니터를 옮긴 그 순간 창을 따라 옮기는 데는 이쪽을 쓴다.
static bool XOutputGeom(Display* dpy, Window root, const std::string& name, int& x, int& y, int& w, int& h) {
  XRRScreenResources* res = XRRGetScreenResourcesCurrent(dpy, root);
  if (!res) return false;
  bool ok = false;
  for (int i = 0; i < res->noutput && !ok; i++) {
    XRROutputInfo* oi = XRRGetOutputInfo(dpy, res, res->outputs[i]);
    if (oi && name == oi->name && oi->crtc) {
      XRRCrtcInfo* ci = XRRGetCrtcInfo(dpy, res, oi->crtc);
      if (ci) { x = ci->x; y = ci->y; w = (int)ci->width; h = (int)ci->height; ok = true; XRRFreeCrtcInfo(ci); }
    }
    if (oi) XRRFreeOutputInfo(oi);
  }
  XRRFreeScreenResources(res);
  return ok;
}

// 반환: true = 화면 해상도 변경으로 다시 만들어야 함, false = 종료/실패
// 반환: 0 = 종료/실패, 1 = 송출 모니터 영역이 바뀌어 다시 만들어야 함, 2 = 송출 모니터가 없어 창을 띄우지 않음(모니터가 생길 때까지 기다림)
[[maybe_unused]] static int PreviewRun() {   // CG_DRM_OUT 빌드에서는 쓰이지 않음
  Display* dpy = XOpenDisplay(nullptr);
  if (!dpy) { fprintf(stderr, "[preview] XOpenDisplay 실패 (DISPLAY 필요)\n"); return 0; }
  int screen = DefaultScreen(dpy);
  int sw = 0, sh = 0, gx = 0, gy = 0;
  if (!ReadOutputGeom(gx, gy, sw, sh)) {   // display.sh 가 기록한 송출 모니터 영역. 없으면 화면 전체를 덮지 않고(다른 모니터를 막지 않도록) 창을 띄우지 않는다
    g_preview_w = 0;
    g_preview_h = 0;
    XCloseDisplay(dpy);
    return 2;
  }
  g_preview_x = gx;
  g_preview_y = gy;
  g_preview_w = sw;
  g_preview_h = sh;

  // 창 종류: 기본은 창 관리자(xfwm4)가 관리하는 정식 창(제목 표시줄 있음). override_redirect 창은 이 단말에서 다른 프로그램의 마우스 클릭이 안 먹는 문제가
  // 있어서(정식 창으로 바꾸니 해결) 쓰지 않는다. CG_PV_STYLE=override(예전 방식)/title(제목 표시줄 있음)/그 외(기본, 제목 표시줄 없는 정식 창).
  const char* pv_style = getenv("CG_PV_STYLE");
  const bool pv_override = pv_style && !strcmp(pv_style, "override");
  const bool pv_borderless = !pv_override && !(pv_style && !strcmp(pv_style, "title"));   // 기본: 창 관리자가 관리하되 제목 표시줄/테두리 없음 (CG_PV_STYLE=title 이면 제목 표시줄 있음)
  XSetWindowAttributes attrs{};
  attrs.override_redirect = pv_override ? True : False;
  attrs.background_pixel = BlackPixel(dpy, screen);
  Window win = XCreateWindow(dpy, RootWindow(dpy, screen), gx, gy, sw, sh, 0,
                              CopyFromParent, InputOutput, CopyFromParent,
                              CWOverrideRedirect | CWBackPixel, &attrs);
  if (!pv_override) {
    XStoreName(dpy, win, "cg-preview");
    XSizeHints sz{};
    sz.flags = USPosition | USSize | PPosition | PSize;   // 위치/크기를 창 관리자에 요청(송출 모니터 영역)
    sz.x = gx; sz.y = gy; sz.width = sw; sz.height = sh;
    XSetWMNormalHints(dpy, win, &sz);
    if (pv_borderless) {
      const Atom mwm = XInternAtom(dpy, "_MOTIF_WM_HINTS", False);
      long mh[5] = {2, 0, 0, 0, 0};   // flags=MWM_HINTS_DECORATIONS, decorations=0 (장식 없음)
      XChangeProperty(dpy, win, mwm, mwm, 32, PropModeReplace, reinterpret_cast<unsigned char*>(mh), 5);
      // 창 종류 DOCK: 창 관리자는 일반 창을 패널(작업 영역) 안으로 밀어 넣어(위쪽에 데스크탑/패널이 보이고 아래가 잘림) 요청한 위치를 못 지킨다.
      // DOCK 은 작업 영역 제한 없이 요청한 위치/크기 그대로 배치되고 포커스를 받지 않는다. 전체화면(FULLSCREEN)은 어느 모니터에 뜰지 창 관리자가 정해서 쓰지 않는다.
      Atom type = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
      XChangeProperty(dpy, win, XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False), XA_ATOM, 32, PropModeReplace, reinterpret_cast<unsigned char*>(&type), 1);
      Atom st[4] = {XInternAtom(dpy, "_NET_WM_STATE_ABOVE", False), XInternAtom(dpy, "_NET_WM_STATE_STICKY", False),
                    XInternAtom(dpy, "_NET_WM_STATE_SKIP_TASKBAR", False), XInternAtom(dpy, "_NET_WM_STATE_SKIP_PAGER", False)};
      XChangeProperty(dpy, win, XInternAtom(dpy, "_NET_WM_STATE", False), XA_ATOM, 32, PropModeReplace, reinterpret_cast<unsigned char*>(st), 4);
    }
    XMapWindow(dpy, win);
    XSync(dpy, False);
    XWindowAttributes wa{};   // 창 관리자가 제목 표시줄만큼 줄였을 수 있으니 자리가 잡힐 때까지 기다린 뒤 실제 크기를 쓴다
    int lw = -1, lh = -1;
    for (int i = 0; i < 20; i++) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      XGetWindowAttributes(dpy, win, &wa);
      if (i >= 2 && wa.width == lw && wa.height == lh) break;
      lw = wa.width; lh = wa.height;
    }
    if (wa.width > 0 && wa.height > 0) { sw = wa.width; sh = wa.height; }
  }
  if (pv_override) XMapRaised(dpy, win);   // (정식 창은 위에서 이미 map 함)
  XFlush(dpy);
  GC gc = XCreateGC(dpy, win, 0, nullptr);
  Visual* visual = DefaultVisual(dpy, screen);
  int depth = DefaultDepth(dpy, screen);

  const bool has_shm = XShmQueryExtension(dpy);
  const int shm_event = has_shm ? XShmGetEventBase(dpy) + ShmCompletion : -1;

  struct Buf { XShmSegmentInfo shm{}; XImage* img = nullptr; bool pending = false; };
  Buf bufs[2];
  std::vector<uint8_t> fallback_buf;   // XShm 없을 때만 사용

  if (has_shm) {
    for (auto& b : bufs) {
      b.img = XShmCreateImage(dpy, visual, depth, ZPixmap, nullptr, &b.shm, sw, sh);
      if (!b.img) continue;
      b.shm.shmid = shmget(IPC_PRIVATE, (size_t)b.img->bytes_per_line * b.img->height, IPC_CREAT | 0600);
      if (b.shm.shmid < 0) { XDestroyImage(b.img); b.img = nullptr; continue; }
      b.shm.shmaddr = b.img->data = (char*)shmat(b.shm.shmid, nullptr, 0);
      b.shm.readOnly = False;
      if (!XShmAttach(dpy, &b.shm)) { XDestroyImage(b.img); b.img = nullptr; continue; }
      shmctl(b.shm.shmid, IPC_RMID, nullptr);   // 프로세스 종료 시 자동 회수되도록 즉시 마킹
    }
  }
  const bool shm_ok = bufs[0].img && bufs[1].img;
  if (!shm_ok) {   // XShm 불가 시 예전 방식(단일 버퍼, XPutImage)으로 대체
    fallback_buf.resize((size_t)sw * sh * 4);
    printf("[preview] XShm 불가 - 일반 XPutImage 로 대체\n");
  }

  using clk = std::chrono::steady_clock;
  uint64_t seen = 0;   // 마지막으로 그린 인코더 프레임 번호
  int cur = 0;
  uint64_t drawn = 0, skipped_pending = 0;   // 진단용: X 서버가 못 따라와서 건너뛴 횟수
  auto last_draw = clk::time_point{};
  double draw_gap_max_ms = 0;                 // 진단용: 그린 시각 사이 최대 간격(ms)
  auto stat_t0 = clk::now();
  printf("[preview] %dx%d 창 시작 (%s)\n", sw, sh, shm_ok ? "XShm 더블버퍼" : "XPutImage");
  // 창을 새 위치로 옮김(다시 만들지 않음). 창 관리자가 요청 위치를 알도록 힌트도 갱신.
  auto move_to = [&](int nx, int ny) {
    if (nx == g_preview_x && ny == g_preview_y) return;
    XSizeHints nsz{};
    nsz.flags = USPosition | USSize | PPosition | PSize;
    nsz.x = nx; nsz.y = ny; nsz.width = sw; nsz.height = sh;
    XSetWMNormalHints(dpy, win, &nsz);
    XMoveWindow(dpy, win, nx, ny);
    XFlush(dpy);
    printf("[preview] 창 위치만 이동 +%d+%d -> +%d+%d (다시 만들지 않음)\n", (int)g_preview_x, (int)g_preview_y, nx, ny);
    g_preview_x = nx;
    g_preview_y = ny;
  };
  // X(RandR)가 송출 모니터를 옮기면(다른 모니터를 뽑거나 꽂을 때) 그 즉시 창도 따라 옮긴다. display.sh 기록 파일/DisplayWatch 는 1.5초 이상 안정된 뒤에야 알아채서,
  // 그 사이 창이 줄어든 화면 밖에 남아 송출 모니터가 검게 보였다(깜빡임).
  int rr_ev = 0, rr_err = 0;
  const bool have_rr = XRRQueryExtension(dpy, &rr_ev, &rr_err);
  if (have_rr) XRRSelectInput(dpy, RootWindow(dpy, screen), RRScreenChangeNotifyMask | RRCrtcChangeNotifyMask | RROutputChangeNotifyMask);
  while (!g_quit) {
    if (have_rr) {
      XEvent rev;
      bool changed = false;
      while (XCheckTypedEvent(dpy, rr_ev + RRScreenChangeNotify, &rev)) { XRRUpdateConfiguration(&rev); changed = true; }
      while (XCheckTypedEvent(dpy, rr_ev + RRNotify, &rev)) changed = true;
      int ox = 0, oy = 0, ow = 0, oh = 0;
      if (changed && XOutputGeom(dpy, RootWindow(dpy, screen), g_output_display, ox, oy, ow, oh) && ow == g_preview_w && oh == g_preview_h) move_to(ox, oy);
    }
    if (g_preview_reset) {
      // 송출 영역의 크기가 그대로이고 위치만 바뀐 경우는 창을 부수지 않고 옮긴다(창을 다시 만들면 그 사이 화면이 비어 깜빡임).
      // 크기가 달라졌거나 모니터가 없어졌으면 예전처럼 창을 다시 만든다.
      int nx = 0, ny = 0, nw = 0, nh = 0;
      if (ReadOutputGeom(nx, ny, nw, nh) && nw == g_preview_w && nh == g_preview_h) {
        move_to(nx, ny);
        g_preview_reset = false;
      } else {
        break;
      }
    }
    {   // 인코더가 새 프레임을 만들 때까지 대기(종료/재생성 확인용으로 50ms 마다 깨어남)
      std::unique_lock<std::mutex> lk(g_pv_mu);
      g_pv_cv.wait_for(lk, std::chrono::milliseconds(50), [&] { return g_pv_seq != seen || g_quit || g_preview_reset; });
      if (g_pv_seq == seen) continue;
      seen = g_pv_seq;
    }

    if (shm_ok) {
      // 이 버퍼가 아직 화면에 표시 중이면(ShmCompletion 미수신) 이번 틱은 건너뛴다(찢어짐 방지).
      if (bufs[cur].pending) {
        XEvent ev;
        while (XCheckTypedEvent(dpy, shm_event, &ev)) {
          XShmCompletionEvent* ce = (XShmCompletionEvent*)&ev;
          for (auto& b : bufs) if (b.shm.shmseg == ce->shmseg) b.pending = false;
        }
      }
      if (bufs[cur].pending) {
        skipped_pending++;   // X 서버가 아직 직전 프레임을 못 그림 -> 이번 프레임은 못 보냄(프레임 드랍)
      } else if (g_enc_ok && g_encoder.ExportPreviewBgrx((uint8_t*)bufs[cur].img->data, sw, sh)) {
        XShmPutImage(dpy, win, gc, bufs[cur].img, 0, 0, 0, 0, sw, sh, True);
        bufs[cur].pending = true;
        XFlush(dpy);
        cur ^= 1;
        drawn++;
        {
          const auto nw = clk::now();
          if (last_draw != clk::time_point{})
            draw_gap_max_ms = std::max(draw_gap_max_ms, std::chrono::duration<double, std::milli>(nw - last_draw).count());
          last_draw = nw;
        }
      }
    } else if (g_enc_ok && g_encoder.ExportPreviewBgrx(fallback_buf.data(), sw, sh)) {
      XImage* tmp = XCreateImage(dpy, visual, depth, ZPixmap, 0, (char*)fallback_buf.data(), sw, sh, 32, 0);
      if (tmp) {
        XPutImage(dpy, win, gc, tmp, 0, 0, 0, 0, sw, sh);
        tmp->data = nullptr;
        XDestroyImage(tmp);
        XFlush(dpy);
        drawn++;
      }
    }
    if (clk::now() - stat_t0 >= std::chrono::seconds(1)) {
      printf("[pstat] drawn=%llu/s skipped_x=%llu/s draw_gap_max=%.0fms\n",
             (unsigned long long)drawn, (unsigned long long)skipped_pending, draw_gap_max_ms);
      drawn = 0; skipped_pending = 0; draw_gap_max_ms = 0; stat_t0 = clk::now();
    }
  }
  for (auto& b : bufs) {
    if (!b.img) continue;
    XShmDetach(dpy, &b.shm);
    XDestroyImage(b.img);
    shmdt(b.shm.shmaddr);
  }
  XFreeGC(dpy, gc);
  XDestroyWindow(dpy, win);
  XCloseDisplay(dpy);
  return (g_preview_reset && !g_quit) ? 1 : 0;
}

#ifdef CG_DRM_OUT
// CG_DRM_OUT 빌드: X 창 대신 송출 모니터(output_display)에 DRM/KMS 로 직접 출력(drm_out.h). 입력(마우스/키보드)은 받지 않는다.
// 반환값 의미는 PreviewRun 과 같다(0 종료, 1 다시 만들기, 2 모니터가 없거나 열기 실패라 reset 을 기다림).
static std::mutex g_drm_sig_mu;
static std::string g_drm_sig;   // 마지막으로 열었을 때의 송출 커넥터 상태(연결/EDID). 바뀌면 다시 연다.

static std::string DrmConnSig() {
  int n = 0;
  if (g_output_display.size() >= 6 && g_output_display.compare(0, 5, "HDMI-") == 0) n = atoi(g_output_display.c_str() + 5);
  if (n < 1) return {};
  const std::string base = "/sys/class/drm/card0-HDMI-A-" + std::to_string(n);
  std::ifstream f(base + "/status");
  std::string v;
  f >> v;
  std::ifstream e(base + "/edid", std::ios::binary);
  const std::string edid((std::istreambuf_iterator<char>(e)), std::istreambuf_iterator<char>());
  return v + ":" + std::to_string(std::hash<std::string>{}(edid) % 100000);
}

static int DrmPreviewRun() {
  { std::lock_guard<std::mutex> lk(g_drm_sig_mu); g_drm_sig = DrmConnSig(); }
  DrmOut out;
  const int r = out.Open(g_output_display);
  fprintf(stderr, "[drm] Open -> %d\n", r);
  if (r != 0) {
    g_preview_w = 0;
    g_preview_h = 0;
    if (r == 2) printf("[drm] 송출 모니터(%s)가 연결되어 있지 않음\n", g_output_display.c_str());
    else fprintf(stderr, "[drm] %s DRM 직접 출력을 열지 못함 - 모니터를 다시 꽂거나 reload 하면 다시 시도\n", g_output_display.c_str());
    return 2;
  }
  g_preview_x = 0;
  g_preview_y = 0;
  g_preview_w = out.width();
  g_preview_h = out.height();

  using clk = std::chrono::steady_clock;
  uint64_t seen = 0, drawn = 0;
  int fails = 0;
  auto stat_t0 = clk::now();
  while (!g_quit && !g_preview_reset) {
    {   // 인코더가 새 프레임을 만들 때까지 대기(종료/재생성 확인용으로 50ms 마다 깨어남)
      std::unique_lock<std::mutex> lk(g_pv_mu);
      g_pv_cv.wait_for(lk, std::chrono::milliseconds(50), [&] { return g_pv_seq != seen || g_quit || g_preview_reset; });
      if (g_pv_seq == seen) continue;
      seen = g_pv_seq;
    }
    if (!g_enc_ok) continue;
    if (out.Present([](int fd, int w, int h, int hs, int vs) { return g_encoder.ExportPreviewNv12(fd, w, h, hs, vs); })) {
      drawn++;
      fails = 0;
    } else if (++fails >= 30) {   // 모니터가 뽑히는 등으로 계속 실패: 접고 reset 을 기다림
      fprintf(stderr, "[drm] 출력이 계속 실패해 중단합니다\n");
      return 2;
    }
    if (clk::now() - stat_t0 >= std::chrono::seconds(1)) {
      printf("[pstat] drm flip=%llu/s\n", (unsigned long long)drawn);
      drawn = 0;
      stat_t0 = clk::now();
    }
  }
  return (g_preview_reset && !g_quit) ? 1 : 0;
}
#endif

static void PreviewLoop() {
  pthread_setname_np(pthread_self(), "cg-preview");
  SetRealtime("cg-preview", 49);
  for (;;) {
    g_preview_reset = false;
#ifdef CG_DRM_OUT
    const int r = DrmPreviewRun();
#else
    const int r = PreviewRun();
#endif
    if (r == 0 || g_quit) break;
    if (r == 2) {   // 송출 모니터가 없음: 창을 띄우지 않고 ApplyDisplay 가 모니터를 찾아 reset 표시를 할 때까지 기다린다
      printf("[preview] 송출 모니터(%s)가 없어 미리보기 창을 띄우지 않음\n", g_output_display.c_str());
      while (!g_quit && !g_preview_reset) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
  }
}

// 로컬 음성을 내보낼 ALSA 장치: 송출 모니터(output_display, 기본 HDMI-2)가 연결되어 있으면 그 쪽 소리 카드, 아니면 연결된 첫 HDMI(모니터가 한 대일 때),
// 하나도 없으면 빈 문자열. HDMI-A-1=rockchiphdmi0, HDMI-A-2=rockchiphdmi1.
static std::string PickAudioDev() {
  auto connected = [](int i) {
    std::ifstream st("/sys/class/drm/card0-HDMI-A-" + std::to_string(i) + "/status");
    std::string s;
    return (st >> s) && s == "connected";
  };
  int want = 0;
  if (g_output_display.size() >= 6 && g_output_display.compare(0, 5, "HDMI-") == 0) want = atoi(g_output_display.c_str() + 5);
  int pick = (want >= 1 && want <= 2 && connected(want)) ? want : 0;
  for (int i = 1; i <= 2 && !pick; i++)
    if (connected(i)) pick = i;
  return pick ? "plughw:CARD=rockchiphdmi" + std::to_string(pick - 1) + ",DEV=0" : std::string();
}

// 로컬 화면 해상도를 다시 맞춘다(bin/display.sh: 연결된 모니터의 권장 모드 + X 화면 크기). 화면 크기가 바뀌었으면 미리보기 창을 다시 만든다.
// reload / Switch project / POST /display 에서 호출(모니터를 바꿔 꽂은 뒤 반영). 별도 스레드에서 실행해 HTTP 처리를 막지 않는다.
static void ApplyDisplay() {
  std::thread([] {
    static std::atomic<bool> busy{false};
    if (busy.exchange(true)) return;
#ifndef CG_DRM_OUT   // DRM 직접 출력 빌드는 송출 모니터를 X 가 쓰지 않아야 하므로 display.sh(송출 모니터를 켬)를 부르지 않는다
    const std::string script = std::filesystem::path(g_exe).parent_path().string() + "/display.sh";
    if (std::filesystem::exists(script)) {
      const std::string cmd = "bash \"" + script + "\" >/dev/null 2>&1";
      if (system(cmd.c_str()) != 0) fprintf(stderr, "[display] display.sh 실행 실패\n");
    }
#endif
    if (g_output_type == 3 && (g_audio_out == "auto" || g_audio_out.empty())) g_audio.ChangeLocalOut(PickAudioDev());   // 소리가 나갈 HDMI 가 바뀌었을 수 있음
#ifdef CG_DRM_OUT
    if (g_preview) {   // 송출 커넥터의 연결/EDID 가 달라졌으면 DRM 출력을 다시 연다
      const std::string sig = DrmConnSig();
      std::lock_guard<std::mutex> lk(g_drm_sig_mu);
      if (sig != g_drm_sig) {
        printf("[display] 송출 모니터 상태 변경(%s -> %s): DRM 출력 다시 열기\n", g_drm_sig.c_str(), sig.c_str());
        g_drm_sig = sig;
        g_preview_reset = true;
      }
    }
#else
    if (g_preview) {   // display.sh 가 기록한 송출 모니터 영역이 지금 미리보기 창과 다르면(또는 모니터가 생기거나 사라졌으면) 창을 다시 만든다
      int x = 0, y = 0, w = 0, h = 0;
      const bool have = ReadOutputGeom(x, y, w, h);
      const bool had = g_preview_w > 0;
      if (have != had || (have && (w != g_preview_w || h != g_preview_h || x != g_preview_x || y != g_preview_y))) {
        printf("[display] 송출 화면 영역 변경 %dx%d+%d+%d -> %dx%d+%d+%d: 미리보기 창 다시 만듦\n", (int)g_preview_w, (int)g_preview_h,
               (int)g_preview_x, (int)g_preview_y, w, h, x, y);
        g_preview_reset = true;
      }
    }
#endif
    busy = false;
  }).detach();
}

// 모니터를 뽑거나 꽂으면(HDMI 커넥터 상태 변화) 배치를 자동으로 다시 계산한다. X 가 연결 변화 때 출력을 모두 0,0 에 겹쳐 놓고 화면을 줄이므로
// (그러면 송출 창이 반쪽만 보임) 1.5초 동안 상태가 안정되면 display.sh 를 다시 돌리고 미리보기 창/음성/키오스크를 맞춘다.
static void DisplayWatch() {
  pthread_setname_np(pthread_self(), "cg-dispwatch");
  auto sig = [] {
    std::string s;
    for (const char* n : {"HDMI-A-1", "HDMI-A-2"}) {
      const std::string base = std::string("/sys/class/drm/card0-") + n;
      std::ifstream f(base + "/status");
      std::string v;
      f >> v;
      // 모니터를 서로 바꿔 꽂아도 두 포트가 계속 connected 로 보일 수 있으므로 모니터의 EDID(제품 식별 정보) 내용도 신호에 넣는다
      std::ifstream e(base + "/edid", std::ios::binary);
      const std::string edid((std::istreambuf_iterator<char>(e)), std::istreambuf_iterator<char>());
      s += v + ":" + std::to_string(std::hash<std::string>{}(edid) % 100000) + ";";
    }
    return s;
  };
  std::string last = sig(), pending;
  int stable = 0;
  using clk = std::chrono::steady_clock;
  std::vector<clk::time_point> recheck;   // 연결 변경 뒤 다시 확인할 시각. 기다리는 동안에도 폴링을 멈추지 않는다(그 사이에 뽑았다 꽂으면 변경을 놓쳐 배치가 겹친 채 남았음)
  while (!g_quit) {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    for (size_t i = 0; i < recheck.size();) {
      if (clk::now() >= recheck[i]) { recheck.erase(recheck.begin() + i); ApplyDisplay(); }
      else i++;
    }
    const std::string cur = sig();
    if (cur == last) { pending.clear(); stable = 0; continue; }
    if (cur == pending) stable++;
    else { pending = cur; stable = 1; }
    if (stable >= 3) {
      last = cur;
      stable = 0;
      pending.clear();
      printf("[display] 모니터 연결 변경 감지(%s): 배치를 다시 계산합니다\n", cur.c_str());
      ApplyDisplay();
      // X 서버는 모니터를 꽂은 뒤 한참 뒤에 두 출력을 모두 0,0 에 겹쳐 놓기도 한다(첫 계산 결과를 덮어씀) -> 3초, 8초 뒤에 한 번씩 더 확인(이미 맞으면 아무것도 안 바뀜)
      recheck = {clk::now() + std::chrono::seconds(3), clk::now() + std::chrono::seconds(8)};
    }
  }
}

static void UdpLoop(CefRefPtr<Client> client) {
  pthread_setname_np(pthread_self(), "cg-udp");
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  timeval tv{0, 200000};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(kCtlPort);
  inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);   // 외부 제어가 필요하면 INADDR_ANY
  if (bind(fd, (sockaddr*)&a, sizeof a) != 0) { perror("udp bind"); close(fd); return; }

  char buf[256];
  while (!g_quit) {
    ssize_t n = recvfrom(fd, buf, sizeof(buf) - 1, 0, nullptr, nullptr);
    if (n <= 0) continue;
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ')) n--;
    buf[n] = 0;
    std::string c(buf);
    std::string js;
    if (c == "quit") g_quit = true;
    else if (c == "next") js = "cg.next()";
    else if (c == "prev") js = "cg.prev()";
    else if (c.rfind("goto ", 0) == 0) js = "cg.goto(" + std::to_string(atoi(c.c_str() + 5)) + ")";
    if (!js.empty()) CefPostTask(TID_UI, base::BindOnce(&Client::Exec, client, js));
  }
  close(fd);
}

// ---------- HTTP 제어 ----------
// POST /play|run|stop|clear|pause|cut|skip|next|prev|reload   POST /play/N  /goto/N (1부터)
//   reload : project 파일을 디스크에서 다시 읽어 1페이지로 돌아감 (에디터에서 저장한 내용 반영)
//   switch : 본문(UTF-8 평문) = bin/project 의 다른 프로젝트 이름(.json 제외) -> 그 파일로 바꿔서 reload
// POST /stamp/{1|2}/{play|stop|pause}                    POST /global/{play|stop}
// PUT  /text/{linkName}  본문 {"text":"..."} (UTF-8)       GET /status      POST /quit
// 명령은 CEF UI 스레드에서 cg.cmd(이름, 인자, 본문) 으로 실행되고, 응답은 접수 결과만 돌려준다.
// JS 문자열은 JsQuote 로 이스케이프해서 넘기므로 요청 내용이 코드로 해석되지 않는다.
// 프로젝트 다시 읽기. 적용된 프로젝트가 없는(대기 중, 프레임 루프가 안 도는) 플레이어는 cg.cmd 로는 시작이 안 되므로 페이지를 새로 불러온다.
static std::string ReloadJs() {
  bool idle;
  { std::lock_guard<std::mutex> lk(g_state_mu); idle = g_state.find("\"ready\":true") == std::string::npos; }
  return idle ? "location.reload()" : "cg.cmd(\"reload\",\"\",\"\")";
}

static std::string JsQuote(const std::string& s) {
  std::string o = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
    else if (c == '\n') o += "\\n";
    else if (c == '\r') o += "\\r";
    else if (c == '\t') o += "\\t";
    else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
    else o += (char)c;
  }
  return o + "\"";
}

static std::vector<std::string> SplitPath(const std::string& p) {
  std::vector<std::string> v;
  std::stringstream ss(p);
  std::string seg;
  while (std::getline(ss, seg, '/'))
    if (!seg.empty()) v.push_back(seg);
  return v;
}

static bool AllDigits(const std::string& s) {
  return !s.empty() && s.size() <= 6 && std::all_of(s.begin(), s.end(), [](unsigned char c) { return isdigit(c); });
}

static bool IsLinkName(const std::string& s) {
  return !s.empty() && s.size() <= 100 &&
         std::all_of(s.begin(), s.end(), [](unsigned char c) { return isalnum(c) || c == '_' || c == '-'; });
}

// 프로젝트 이름(한글 포함 UTF-8). 경로 구분자/상위 경로/제어문자만 막는다(cg-editor 의 safeName 과 같은 취지).
static bool SafeProjectName(const std::string& s) {
  if (s.empty() || s.size() > 200 || s == "." || s == "..") return false;
  return std::none_of(s.begin(), s.end(), [](unsigned char c) { return c == '/' || c == '\\' || c < 0x20; });
}

static void HttpReply(int fd, int code, const std::string& body) {
  const char* reason = code == 200 ? "OK" : code == 400 ? "Bad Request" : code == 401 ? "Unauthorized" :
                       code == 404 ? "Not Found" : code == 405 ? "Method Not Allowed" : "Payload Too Large";
  std::string r = "HTTP/1.1 " + std::to_string(code) + " " + reason + "\r\nContent-Type: application/json; charset=utf-8\r\n" +
                  "Content-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
  send(fd, r.data(), r.size(), MSG_NOSIGNAL);
}

// 요청 1개 처리. 잘못된 요청은 4xx 로 응답.
static void HttpHandle(int fd, CefRefPtr<Client> client) {
  constexpr size_t kMaxHead = 16 * 1024, kMaxBody = 64 * 1024;
  std::string req;
  char buf[4096];
  size_t head_end = std::string::npos, need = 0;
  while (head_end == std::string::npos || req.size() < need) {
    ssize_t n = recv(fd, buf, sizeof buf, 0);
    if (n <= 0) return;                               // 타임아웃/끊김
    req.append(buf, n);
    if (head_end == std::string::npos) {
      head_end = req.find("\r\n\r\n");
      if (head_end == std::string::npos) { if (req.size() > kMaxHead) return HttpReply(fd, 400, "{\"ok\":false,\"error\":\"header too large\"}"); continue; }
      head_end += 4;
      need = head_end;
      std::string lower = req.substr(0, head_end);
      std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return tolower(c); });
      size_t p = lower.find("\r\ncontent-length:");
      if (p != std::string::npos) {
        size_t len = strtoull(lower.c_str() + p + 17, nullptr, 10);
        if (len > kMaxBody) return HttpReply(fd, 413, "{\"ok\":false,\"error\":\"body too large\"}");
        need += len;
      }
    }
  }

  const size_t eol = req.find("\r\n");
  std::stringstream rl(req.substr(0, eol));
  std::string method, target, ver;
  rl >> method >> target >> ver;
  const std::string head = req.substr(0, head_end), body = req.substr(head_end, need - head_end);
  std::string path = target.substr(0, target.find('?'));

  if (!g_http_token.empty()) {
    std::string lower = head;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return tolower(c); });
    size_t p = lower.find("\r\nauthorization:");
    std::string got;
    if (p != std::string::npos) {
      size_t e = head.find("\r\n", p + 2);
      got = head.substr(p + 16, e - (p + 16));
      while (!got.empty() && got[0] == ' ') got.erase(0, 1);
    }
    if (got != "Bearer " + g_http_token) return HttpReply(fd, 401, "{\"ok\":false,\"error\":\"unauthorized\"}");
  }

  const auto seg = SplitPath(path);
  if (seg.empty()) return HttpReply(fd, 404, "{\"ok\":false,\"error\":\"not found\"}");

  if (seg[0] == "status" && seg.size() == 1) {
    if (method != "GET") return HttpReply(fd, 405, "{\"ok\":false,\"error\":\"use GET\"}");
    std::string s;
    { std::lock_guard<std::mutex> lk(g_state_mu); s = g_state; }
    std::error_code ec;   // 현재 적용된 프로젝트 파일 이름(.json 제외). 프로젝트 안의 name 과 다를 수 있어 에디터가 목록에서 고를 때 쓴다.
    if (s.size() > 2 && s.back() == '}' && std::filesystem::exists(g_project, ec))
      s.insert(s.size() - 1, ",\"file\":" + JsQuote(std::filesystem::path(g_project).stem().string()));
    return HttpReply(fd, 200, s);
  }
  if (method != (seg[0] == "text" ? "PUT" : "POST")) return HttpReply(fd, 405, "{\"ok\":false,\"error\":\"method not allowed\"}");

  std::string name = seg[0], arg, payload;
  bool ok = false;
  if (seg.size() == 1 && name == "quit") {
    g_quit = true;
    return HttpReply(fd, 200, "{\"ok\":true}");
  } else if (seg.size() == 1 && name == "switch") {   // 본문(UTF-8 평문) = bin/project 의 다른 프로젝트 이름(.json 제외)
    if (!SafeProjectName(body)) return HttpReply(fd, 400, "{\"ok\":false,\"error\":\"invalid project name\"}");
    const auto np = std::filesystem::path(g_project).parent_path() / (body + ".json");
    if (!std::filesystem::exists(np)) return HttpReply(fd, 404, "{\"ok\":false,\"error\":\"project not found\"}");
    g_project = np.string();
    {   // 마지막으로 적용한 프로젝트를 기록: start.sh 가 다음 시작 때 이 프로젝트로 띄운다(.run 은 bin/project 의 상위 = bin/.run)
      std::error_code ec;
      const auto run_dir = np.parent_path().parent_path() / ".run";
      std::filesystem::create_directories(run_dir, ec);
      std::ofstream(run_dir / "last-project", std::ios::binary | std::ios::trunc) << body;
    }
    ApplyDisplay();   // 모니터를 바꿔 꽂았을 수 있으니 해상도도 다시 맞춤
    CefPostTask(TID_UI, base::BindOnce(&Client::Exec, client, ReloadJs()));
    return HttpReply(fd, 200, "{\"ok\":true}");
  } else if (seg.size() == 1 && name == "display") {   // 로컬 화면 해상도만 다시 맞춤
    ApplyDisplay();
    return HttpReply(fd, 200, "{\"ok\":true}");
  } else if (seg.size() == 1 && (name == "play" || name == "run" || name == "stop" || name == "clear" || name == "pause" ||
                                 name == "cut" || name == "skip" || name == "next" || name == "prev" || name == "reload")) {
    if (name == "reload") ApplyDisplay();   // Reload saved project 때도 해상도 다시 맞춤
    ok = true;
  } else if (seg.size() == 2 && (name == "play" || name == "goto") && AllDigits(seg[1])) {
    arg = seg[1]; ok = true;
  } else if (seg.size() == 3 && name == "stamp" && (seg[1] == "1" || seg[1] == "2") &&
             (seg[2] == "play" || seg[2] == "stop" || seg[2] == "pause")) {
    arg = seg[1] + "/" + seg[2]; ok = true;
  } else if (seg.size() == 2 && name == "global" && (seg[1] == "play" || seg[1] == "stop")) {
    arg = seg[1]; ok = true;
  } else if (seg.size() == 2 && name == "text" && IsLinkName(seg[1])) {
    arg = seg[1]; payload = body; ok = true;
  }
  if (!ok) return HttpReply(fd, 404, "{\"ok\":false,\"error\":\"unknown command\"}");

  const std::string js = name == "reload" ? ReloadJs() : "cg.cmd(" + JsQuote(name) + "," + JsQuote(arg) + "," + JsQuote(payload) + ")";
  CefPostTask(TID_UI, base::BindOnce(&Client::Exec, client, js));
  HttpReply(fd, 200, "{\"ok\":true}");
}

static void HttpLoop(CefRefPtr<Client> client) {
  pthread_setname_np(pthread_self(), "cg-http");
  int ls = socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(g_http_port);
  if (inet_pton(AF_INET, g_http_bind.c_str(), &a.sin_addr) != 1) {
    fprintf(stderr, "[http] 잘못된 --bind 주소: %s\n", g_http_bind.c_str());
    close(ls);
    return;
  }
  if (bind(ls, (sockaddr*)&a, sizeof a) != 0 || listen(ls, 16) != 0) {
    perror("[http] bind/listen");
    close(ls);
    return;
  }
  while (!g_quit) {
    pollfd pf{ls, POLLIN, 0};
    if (poll(&pf, 1, 200) <= 0) continue;
    int fd = accept(ls, nullptr, nullptr);
    if (fd < 0) continue;
    timeval tv{2, 0};                                  // 느린 클라이언트가 서버를 붙잡지 못하게
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    HttpHandle(fd, client);
    close(fd);
  }
  close(ls);
}

static void OnSignal(int) { g_quit = true; }

// --run 중복 실행 방지: 같은 하드웨어 인코더/UDP 목적지/컨트롤 포트를 놓고 여러 인스턴스가
// 동시에 떠 있으면 출력이 서로 섞여 비트레이트가 이상해진다. flock 은 fd 가 열려 있는 동안만
// 유지되므로 비정상 종료해도 커널이 알아서 풀어준다 (별도 정리 불필요).
static int g_run_lock_fd = -1;
static bool AcquireRunLock() {
  g_run_lock_fd = open("/tmp/cg-streamer.run.lock", O_CREAT | O_RDWR, 0644);
  if (g_run_lock_fd < 0) return true;  // 잠금 파일을 못 만들면(권한 등) 그냥 진행
  if (flock(g_run_lock_fd, LOCK_EX | LOCK_NB) == 0) {
    if (ftruncate(g_run_lock_fd, 0) == 0) {
      std::string pid = std::to_string(getpid()) + "\n";
      if (write(g_run_lock_fd, pid.data(), pid.size()) < 0) { /* 참고용 PID 기록 실패는 무시 */ }
    }
    return true;
  }
  return false;
}

int main(int argc, char* argv[]) {
  g_video.SetAudio(&g_audio);
  g_hdmi.SetAudio(&g_audio);
  CefMainArgs main_args(argc, argv);
  CefRefPtr<App> app = new App;

  // 서브프로세스(렌더러 등)면 여기서 종료. g_app_mode 는 브라우저 프로세스에서만 필요.
  for (int i = 1; i < argc; i++)
    if (!strcmp(argv[i], "--run")) g_app_mode = AppMode::kRun;
  int code = CefExecuteProcess(main_args, app, nullptr);
  if (code >= 0) return code;

  if (g_app_mode == AppMode::kRun) {   // 이후 모든 printf/stderr(CEF 자식 포함)를 시간 붙여 일별 파일(log/cg-streamer-YYYY-MM-DD.log)로
    std::error_code ec;
    const auto exe_dir = std::filesystem::read_symlink("/proc/self/exe", ec).parent_path();
    LogWriterStart((ec ? std::filesystem::path(".") : exe_dir) / "log", "cg-streamer", 30);
  }

  for (int i = 1; i < argc; i++)
    if (!strncmp(argv[i], "--setup=", 8)) g_setup_cfg = argv[i] + 8;
  LoadSetupCfg(g_setup_cfg);
  g_out = "udp://" + g_udp_ip + ":" + std::to_string(g_udp_port) + "?pkt_size=1316";
  g_paint_fps = g_fps;   // cfg 의 fps 를 CEF 페인트 fps 기본값에도 반영 (--paint-fps= 로 다시 덮어쓸 수 있음)
  if (g_output_type == 1) { g_view = true; g_encode = false; }  // HDMI 직결만: 로컬 전체화면, 인코딩/UDP 없음
  else if (g_output_type == 3) g_preview = true;                // HDMI+UDP 동시: 인코딩 결과를 로컬 창에도 표시
  if (g_output_type == 3 && g_audio_out != "off") {   // HDMI+UDP 동시: 송출과 같은 음성을 로컬 HDMI 로도 재생
    // 장치가 아직 없어도(빈 문자열) 로컬 재생은 켜 두고, 모니터가 연결되면 ChangeLocalOut 으로 바뀐다
    g_audio.SetLocalOut(g_audio_out == "auto" || g_audio_out.empty() ? PickAudioDev() : g_audio_out);
  }

  int seconds = 0;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a.rfind("--project=", 0) == 0) g_project = a.substr(10);
    else if (a.rfind("--out=", 0) == 0) g_out = a.substr(6);
    else if (a.rfind("--udp=", 0) == 0) g_out = "udp://" + a.substr(6) + "?pkt_size=1316";
    else if (a.rfind("--seconds=", 0) == 0) seconds = atoi(a.c_str() + 10);
    else if (a.rfind("--http=", 0) == 0) g_http_port = std::max(1, std::min(65535, atoi(a.c_str() + 7)));
    else if (a.rfind("--bind=", 0) == 0) g_http_bind = a.substr(7);
    else if (a.rfind("--token=", 0) == 0) g_http_token = a.substr(8);
    else if (a == "--autoplay") g_autoplay = true;
    else if (a == "--jsprof") g_jsprof = true;
    else if (a == "--accel") g_paint_mode = PaintMode::kAccel;
    else if (a == "--gpu") g_gpu_composite = true;
    else if (a == "--no-encode") g_encode = false;
    else if (a == "--no-sync") g_sync = false;
    else if (a == "--view") g_view = true, g_encode = false;
    else if (a == "--preview") g_preview = true;
    else if (a.rfind("--paint-fps=", 0) == 0) g_paint_fps = std::max(1, std::min(g_fps, atoi(a.c_str() + 12)));
    else if (a.rfind("--page=", 0) == 0) g_page = a.substr(7);
#if 0
    else if (a.rfind("--dump-onpaint=", 0) == 0) g_dump_path = a.substr(15);
    else if (a.rfind("--dump-seconds=", 0) == 0) g_dump_seconds = std::max(1, atoi(a.c_str() + 15));
#endif
    else if (a.rfind("--cef:", 0) == 0) g_extra.push_back(a.substr(6));
  }
#ifndef CG_ACCEL
  if (g_paint_mode == PaintMode::kAccel) {
    fprintf(stderr, "--accel 은 -DENABLE_ACCEL_PAINT=ON 으로 빌드해야 합니다. OnPaint 로 진행.\n");
    g_paint_mode = PaintMode::kSoftware;
  }
#endif
  namespace fs = std::filesystem;
  g_project = fs::absolute(g_project).string();
  if (g_out.find("://") == std::string::npos) g_out = fs::absolute(g_out).string();  // 파일이면 절대경로
  g_exe = fs::read_symlink("/proc/self/exe").string();
  g_webdir = fs::path(g_exe).parent_path().string() + "/web";

  const bool run_mode = (g_app_mode == AppMode::kRun);
  if (run_mode && !AcquireRunLock()) {
    fprintf(stderr, "[cg] 이미 다른 cg-streamer --run 인스턴스가 실행 중입니다. "
                     "먼저 종료하세요 (예: curl -X POST http://127.0.0.1:5555/quit).\n");
    return 1;
  }
  const bool osr = run_mode && !g_view;

  CefSettings settings;
  settings.no_sandbox = true;
  settings.windowless_rendering_enabled = osr;
  if (!CefInitialize(main_args, settings, app, nullptr)) return 1;

  g_router = CefMessageRouterBrowserSide::Create(CefMessageRouterConfig());
  CefRefPtr<Client> client = new Client(osr);
  g_router->AddHandler(client.get(), false);

  std::thread enc_thread, udp_thread, http_thread, watcher, preview_thread, display_thread;

  if (g_app_mode == AppMode::kRun) {
    // ===== [RUN MODE] paint=kSoftware(기본) 또는 kAccel(--accel, 실험) =====
    signal(SIGINT, OnSignal);
    signal(SIGTERM, OnSignal);

    if (g_view) {   // --view: 같은 player.html 을 전체화면 창으로 (OnPaint/인코딩 없음)
      CefBrowserSettings bs;
      auto view = CefBrowserView::CreateBrowserView(
          client, "file://" + g_webdir + "/" + g_page + (g_autoplay ? "?autoplay=1" : ""), bs, nullptr, nullptr, nullptr);
      CefWindow::CreateTopLevelWindow(new WinDelegate(view, true));
    } else {
    CefWindowInfo wi;
    wi.SetAsWindowless(0);
    wi.runtime_style = CEF_RUNTIME_STYLE_ALLOY;  // CEF 125+ : OSR은 Alloy 스타일 필요
#ifdef CG_ACCEL
    wi.shared_texture_enabled = (g_paint_mode == PaintMode::kAccel);  // dmabuf 로 OnAcceleratedPaint 호출
#endif
#ifdef CG_EBF
    wi.external_begin_frame_enabled = !g_view;  // CEF 자체 타이머 끔: SendExternalBeginFrame 으로만 그려짐
#endif
    CefBrowserSettings bs;
    bs.windowless_frame_rate = g_paint_fps;
    bs.background_color = CefColorSetARGB(0, 0, 0, 0);   // 투명: 페이지 배경이 없으면 영상이 비침
    CefBrowserHost::CreateBrowser(wi, client, "file://" + g_webdir + "/" + g_page + PlayerQuery(), bs,
                                  nullptr, nullptr);
    }
#if 0
    if (!g_dump_path.empty() && g_dumper.Open(g_dump_path, kW, kH, g_paint_fps))
      g_dump_t_end = std::chrono::steady_clock::now() + std::chrono::seconds(g_dump_seconds);
#endif

    if (g_encode) enc_thread = std::thread(EncodeLoop, client);
    if (g_preview) preview_thread = std::thread(PreviewLoop);
    if (g_preview || g_output_type == 1) display_thread = std::thread(DisplayWatch);   // 모니터를 뽑거나 꽂으면 배치를 자동으로 다시 계산
    udp_thread = std::thread(UdpLoop, client);
    http_thread = std::thread(HttpLoop, client);
    watcher = std::thread([&] {
      pthread_setname_np(pthread_self(), "cg-watch");
      auto t0 = std::chrono::steady_clock::now();
      auto last_stat = t0;
      uint64_t last_paints = 0;
      while (!g_quit) {
#ifdef CG_EBF
        if (!g_encode && !g_view) {
          std::this_thread::sleep_for(std::chrono::nanoseconds(1000000000LL / g_paint_fps));
          client->RequestBeginFrame();
        } else
#endif
        { std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
        const auto now = std::chrono::steady_clock::now();
        if (!g_encode && !g_view && now - last_stat >= std::chrono::seconds(1)) {
          last_stat = now;   // --no-encode: EncodeLoop 대신 paint 통계
          uint64_t p = g_paints.load();
          printf("[estat] no-encode paint=%llu/s\n", (unsigned long long)(p - last_paints));
          last_paints = p;
        }
        if (seconds > 0 && std::chrono::steady_clock::now() - t0 > std::chrono::seconds(seconds))
          g_quit = true;
      }
      CefPostTask(TID_UI, base::BindOnce(&Client::Close, client));
    });
#ifdef CG_EBF
    if (!g_view) printf("[cg] frame clock: external begin frame (%dfps, UI-thread requests)\n", g_paint_fps);
#endif
    printf("[cg] run: project=%s out=%s paint=%s paint_fps=%d sync=%s (제어: HTTP %s:%d%s, UDP %d next|prev|goto N|quit)\n",
           g_project.c_str(), g_out.c_str(),
           g_view ? "view" : g_paint_mode == PaintMode::kAccel ? "accel" : g_gpu_composite ? "software+gpu" : "software",
           g_paint_fps, g_sync && g_paint_fps == g_fps ? "jitter-buffer(5)" : "latest", g_http_bind.c_str(), g_http_port,
           g_http_token.empty() ? "" : " (token)", kCtlPort);
  } else {
    // ===== [EDIT MODE] =====
    CefBrowserSettings bs;
    auto view = CefBrowserView::CreateBrowserView(
        client, "file://" + g_webdir + "/editor.html", bs, nullptr, nullptr, nullptr);
    CefWindow::CreateTopLevelWindow(new WinDelegate(view));
  }

  CefRunMessageLoop();

  g_quit = true;
  if (watcher.joinable()) watcher.join();
  if (udp_thread.joinable()) udp_thread.join();
  if (http_thread.joinable()) http_thread.join();
  if (enc_thread.joinable()) enc_thread.join();
  if (preview_thread.joinable()) preview_thread.join();
  if (display_thread.joinable()) display_thread.join();
  g_video.Stop();
  g_hdmi.Stop();
  g_encoder.Deinit();
  g_router->RemoveHandler(client.get());
  g_router = nullptr;
  client = nullptr;
  CefShutdown();
  if (run_mode) printf("stopped: %s\n", g_out.c_str());
  return 0;
}
