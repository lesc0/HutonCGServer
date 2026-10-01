// 문자발생기(CG): 저작(--edit) / 실행(--run)
//   ./cg-streamer --edit  [--project=project.json]
//   ./cg-streamer --run   [--project=project.json] [--udp=host:port | --out=URL또는파일.ts] [--seconds=N]
//   실험: cmake -DENABLE_ACCEL_PAINT=ON 빌드 후 --accel (OnAcceleratedPaint dmabuf, GPU 경로)
//         --gpu : OnPaint 경로 그대로, GPU 합성만 켬 (disable-gpu 생략, 예: --cef:use-angle=gles-egl)
//   --http=PORT : HTTP 제어 포트 (기본 5555)  --bind=ADDR (기본 127.0.0.1)  --token=문자열 (Authorization: Bearer)
//   --autoplay : 로딩 후 Run Setting 시작 페이지부터 자동 재생 (기본은 출력을 비워 두고 명령 대기)
//   --view : 전체화면 창으로 재생 (DISPLAY 필요, 인코딩 없음)
//   --preview : 인코딩(UI+영상 합성)은 그대로 하면서, 그 결과를 별도 X11 창(DISPLAY 필요)에도 표시
//   --no-encode : 렌더링만 (인코딩/전송 없음, CEF 자체 부하 측정용)
//   --no-sync : UI 지터 버퍼 끔 (항상 최신 그림 — 지연 0 이지만 CEF 박자와 어긋나 0/2프레임씩 끊김)
//   --paint-fps=N : CEF 렌더링 fps (기본 60). 인코딩/출력은 60fps 유지(직전 프레임 반복)
//         추가 크로미움 스위치: --cef:이름[=값]  (예: --cef:use-angle=gles)
//   --setup=cgsetup.cfg : 송출 설정 파일 (기본 ./cgsetup.cfg, 없으면 기본값 사용)
//         output=1(HDMI만) 2(UDP만, 기본) 3(HDMI+UDP) / udp_ip=... / udp_port=...
//         커맨드라인의 --udp=/--out=/--view/--preview 는 이 파일보다 항상 우선
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
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <chrono>
#include <cstdio>
#include <cctype>
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

// X11 은 CEF 헤더 뒤에 포함(Success/None 등 매크로가 CEF의 동명 심볼과 충돌).
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#undef Success

extern char** environ;

static constexpr int kW = 1920, kH = 1080, kFps = 60;
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
static int g_paint_fps = kFps;                   // --paint-fps=N: CEF 렌더링 fps (인코딩은 kFps 유지)
static bool g_sync = true;                       // UI 지터 버퍼 사용 (--no-sync: 최신 그림만)

static std::string g_project = "project.json";
static std::string g_out = "udp://127.0.0.1:1234?pkt_size=1316";
static std::string g_exe, g_webdir;

// cgsetup.cfg: output=1(HDMI만) 2(UDP만, 기본) 3(HDMI+UDP) / udp_ip / udp_port
// 입력(HDMI RX)은 자동 감지이므로 설정 대상 아님. --setup= 로 경로 변경 가능, 커맨드라인 인자가 항상 우선.
static std::string g_setup_cfg = "cgsetup.cfg";
static int g_output_type = 2;
static std::string g_udp_ip = "127.0.0.1";
static int g_udp_port = 1234;

// HTTP 제어 (--http=PORT --bind=ADDR --token=TOKEN). 기본은 loopback 전용, 토큰 없음.
static int g_http_port = kCtlPort;               // TCP 와 UDP 포트는 별개라 같은 번호를 써도 충돌하지 않는다
static std::string g_http_bind = "127.0.0.1";
static std::string g_http_token;
static bool g_autoplay = false;                  // --autoplay: 로딩 후 Run Setting 시작 페이지부터 자동 재생
static std::mutex g_state_mu;                    // player 가 cefQuery('state:...') 로 올려 주는 상태 (GET /status)
static std::string g_state = "{\"ready\":false}";
static std::atomic<bool> g_ready{false};   // player.html 로딩 완료 신호
static std::atomic<bool> g_quit{false};
static std::atomic<uint64_t> g_paints{0};
static pid_t g_child = 0;
static CefRefPtr<CefMessageRouterBrowserSide> g_router;

// UI 지터 버퍼: CEF 는 자체 60Hz 로 그리고 인코더는 별도 60Hz 타이머로 돈다. 두 박자가 어긋나면
// "같은 그림 2번 / 1장 건너뜀"이 생겨 티커 등이 끊겨 보이므로, 그림을 3장 쌓아 두고(지연 약 50ms)
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
    if (!g_encode) { g_paints++; return; }   // --no-encode: 복사도 생략
    std::lock_guard<std::mutex> lk(g_store.m);
    UiFrame f;
    if (!g_store.pool.empty()) { f.px = std::move(g_store.pool.back()); g_store.pool.pop_back(); }
    f.px.resize((size_t)w * h * 4);
    memcpy(f.px.data(), buffer, (size_t)w * h * 4);  // BGRA
    f.seq = ++g_store.seq;
    f.at = std::chrono::steady_clock::now();
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

  // --- JS -> 네이티브 (cefQuery) ---
  bool OnQuery(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>, int64_t, const CefString& request,
               bool, CefRefPtr<Callback> cb) override {
    const std::string r = request.ToString();

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
static void EncodeLoop() {
  pthread_setname_np(pthread_self(), "cg-encode");
  if (!g_encoder.Init(kW, kH, kFps, kBitrate)) { g_quit = true; return; }
  g_enc_ok = true;
  TsMuxer mux;
  if (!mux.Open(g_out, kW, kH, kFps)) { g_quit = true; return; }

  using clk = std::chrono::steady_clock;
  const auto period = std::chrono::nanoseconds(1000000000LL / kFps);
  auto next = clk::now();
  uint64_t frames = 0, bytes = 0, last_paints = 0, converted_seq = 0, uploaded_seq = 0, underflows = 0;
  const bool jitter_buf = g_sync && g_paint_fps == kFps;   // --paint-fps 가 60 미만이면 최신 그림 방식
  UiFrame cur;                  // 현재 출력 중인 UI 그림 (EncodeLoop 소유)
  bool primed = false;
  auto over_since = clk::time_point{};   // 큐가 목표 수위를 넘은 시점 (1초 이상 지속되면 1장 버려 지연 복귀)
  auto t0 = clk::now();

  while (!g_quit) {
    if (!g_ready) {               // 로딩 중 화면은 인코딩하지 않음 (렌더링은 계속 요청해야 ready 가 옴)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      next = clk::now();
      t0 = next;
      continue;
    }
    next += period;
    std::this_thread::sleep_until(next);

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
        // 2장 쌓였거나, 1장이라도 2프레임 넘게 기다렸으면(정지 화면 등) 출력 시작
        if (!primed && !g_store.q.empty() &&
            (g_store.q.size() >= kUiPrime || clk::now() - g_store.q.front().at >= period * 2))
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
    if (!g_encoder.Encode([&](const uint8_t* d, size_t n) { mux.Write(d, n); bytes += n; }))
      continue;                     // 아직 프레임 없음
    frames++;

    if (frames % kFps == 0) {
      double sec = std::chrono::duration<double>(clk::now() - t0).count();
      uint64_t p = g_paints.load();
      size_t ql;
      uint64_t dropped;
      {
        std::lock_guard<std::mutex> lk(g_store.m);
        ql = g_store.q.size();
        dropped = g_store.dropped;
      }
      printf("[stat] enc=%.1ffps paint=%llu/s out=%.2fMbps rga=%.1fms video=%s uiq=%zu under=%llu drop=%llu accel_fail=%llu\n",
             frames / sec, (unsigned long long)(p - last_paints), bytes * 8.0 / sec / 1e6, g_encoder.TakeComposeMs(),
             g_hdmi.Active() ? (g_hdmi.HasSignal() ? "hdmirx" : "hdmirx(no-signal)")
                             : g_video.Active() ? "on" : "off", ql, (unsigned long long)underflows,
             (unsigned long long)dropped, (unsigned long long)g_accel_fail.load());
      last_paints = p;
      frames = 0; bytes = 0; t0 = clk::now();
    }
  }
  g_enc_ok = false;
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
static void PreviewLoop() {
  pthread_setname_np(pthread_self(), "cg-preview");
  Display* dpy = XOpenDisplay(nullptr);
  if (!dpy) { fprintf(stderr, "[preview] XOpenDisplay 실패 (DISPLAY 필요)\n"); return; }
  int screen = DefaultScreen(dpy);
  int sw = DisplayWidth(dpy, screen), sh = DisplayHeight(dpy, screen);

  XSetWindowAttributes attrs{};
  attrs.override_redirect = True;
  attrs.background_pixel = BlackPixel(dpy, screen);
  Window win = XCreateWindow(dpy, RootWindow(dpy, screen), 0, 0, sw, sh, 0,
                              CopyFromParent, InputOutput, CopyFromParent,
                              CWOverrideRedirect | CWBackPixel, &attrs);
  XMapRaised(dpy, win);
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
  const auto period = std::chrono::milliseconds(1000 / kFps);   // 원본과 동일한 60fps
  auto next = clk::now();
  int cur = 0;
  uint64_t drawn = 0, skipped_pending = 0;   // 진단용: X 서버가 못 따라와서 건너뛴 횟수
  auto stat_t0 = clk::now();
  printf("[preview] %dx%d 창 시작 (%s)\n", sw, sh, shm_ok ? "XShm 더블버퍼" : "XPutImage");
  while (!g_quit) {
    next += period;

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
      printf("[preview-stat] drawn=%llu/s skipped(X못따라옴)=%llu/s\n",
             (unsigned long long)drawn, (unsigned long long)skipped_pending);
      drawn = 0; skipped_pending = 0; stat_t0 = clk::now();
    }
    std::this_thread::sleep_until(next);
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
    std::lock_guard<std::mutex> lk(g_state_mu);
    return HttpReply(fd, 200, g_state);
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
    CefPostTask(TID_UI, base::BindOnce(&Client::Exec, client, std::string("cg.cmd(\"reload\",\"\",\"\")")));
    return HttpReply(fd, 200, "{\"ok\":true}");
  } else if (seg.size() == 1 && (name == "play" || name == "run" || name == "stop" || name == "clear" || name == "pause" ||
                                 name == "cut" || name == "skip" || name == "next" || name == "prev" || name == "reload")) {
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

  const std::string js = "cg.cmd(" + JsQuote(name) + "," + JsQuote(arg) + "," + JsQuote(payload) + ")";
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
  CefMainArgs main_args(argc, argv);
  CefRefPtr<App> app = new App;

  // 서브프로세스(렌더러 등)면 여기서 종료. g_app_mode 는 브라우저 프로세스에서만 필요.
  for (int i = 1; i < argc; i++)
    if (!strcmp(argv[i], "--run")) g_app_mode = AppMode::kRun;
  int code = CefExecuteProcess(main_args, app, nullptr);
  if (code >= 0) return code;

  for (int i = 1; i < argc; i++)
    if (!strncmp(argv[i], "--setup=", 8)) g_setup_cfg = argv[i] + 8;
  LoadSetupCfg(g_setup_cfg);
  g_out = "udp://" + g_udp_ip + ":" + std::to_string(g_udp_port) + "?pkt_size=1316";
  if (g_output_type == 1) { g_view = true; g_encode = false; }  // HDMI 직결만: 로컬 전체화면, 인코딩/UDP 없음
  else if (g_output_type == 3) g_preview = true;                // HDMI+UDP 동시: 인코딩 결과를 로컬 창에도 표시

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
    else if (a == "--accel") g_paint_mode = PaintMode::kAccel;
    else if (a == "--gpu") g_gpu_composite = true;
    else if (a == "--no-encode") g_encode = false;
    else if (a == "--no-sync") g_sync = false;
    else if (a == "--view") g_view = true, g_encode = false;
    else if (a == "--preview") g_preview = true;
    else if (a.rfind("--paint-fps=", 0) == 0) g_paint_fps = std::max(1, std::min(kFps, atoi(a.c_str() + 12)));
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

  std::thread enc_thread, udp_thread, http_thread, watcher, preview_thread;

  if (g_app_mode == AppMode::kRun) {
    // ===== [RUN MODE] paint=kSoftware(기본) 또는 kAccel(--accel, 실험) =====
    signal(SIGINT, OnSignal);
    signal(SIGTERM, OnSignal);

    if (g_view) {   // --view: 같은 player.html 을 전체화면 창으로 (OnPaint/인코딩 없음)
      CefBrowserSettings bs;
      auto view = CefBrowserView::CreateBrowserView(
          client, "file://" + g_webdir + "/player.html" + (g_autoplay ? "?autoplay=1" : ""), bs, nullptr, nullptr, nullptr);
      CefWindow::CreateTopLevelWindow(new WinDelegate(view, true));
    } else {
    CefWindowInfo wi;
    wi.SetAsWindowless(0);
    wi.runtime_style = CEF_RUNTIME_STYLE_ALLOY;  // CEF 125+ : OSR은 Alloy 스타일 필요
#ifdef CG_ACCEL
    wi.shared_texture_enabled = (g_paint_mode == PaintMode::kAccel);  // dmabuf 로 OnAcceleratedPaint 호출
#endif
    CefBrowserSettings bs;
    bs.windowless_frame_rate = g_paint_fps;
    bs.background_color = CefColorSetARGB(0, 0, 0, 0);   // 투명: 페이지 배경이 없으면 영상이 비침
    CefBrowserHost::CreateBrowser(wi, client, "file://" + g_webdir + "/player.html" + (g_autoplay ? "?autoplay=1" : ""), bs,
                                  nullptr, nullptr);
    }

    if (g_encode) enc_thread = std::thread(EncodeLoop);
    if (g_preview) preview_thread = std::thread(PreviewLoop);
    udp_thread = std::thread(UdpLoop, client);
    http_thread = std::thread(HttpLoop, client);
    watcher = std::thread([&] {
      pthread_setname_np(pthread_self(), "cg-watch");
      auto t0 = std::chrono::steady_clock::now();
      int ticks = 0;
      uint64_t last_paints = 0;
      while (!g_quit) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!g_encode && !g_view && ++ticks % 10 == 0) {   // --no-encode: EncodeLoop 대신 paint 통계
          uint64_t p = g_paints.load();
          printf("[stat] no-encode paint=%llu/s\n", (unsigned long long)(p - last_paints));
          last_paints = p;
        }
        if (seconds > 0 && std::chrono::steady_clock::now() - t0 > std::chrono::seconds(seconds))
          g_quit = true;
      }
      CefPostTask(TID_UI, base::BindOnce(&Client::Close, client));
    });
    printf("[cg] run: project=%s out=%s paint=%s paint_fps=%d sync=%s (제어: HTTP %s:%d%s, UDP %d next|prev|goto N|quit)\n",
           g_project.c_str(), g_out.c_str(),
           g_view ? "view" : g_paint_mode == PaintMode::kAccel ? "accel" : g_gpu_composite ? "software+gpu" : "software",
           g_paint_fps, g_sync && g_paint_fps == kFps ? "jitter-buffer(3)" : "latest", g_http_bind.c_str(), g_http_port,
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
