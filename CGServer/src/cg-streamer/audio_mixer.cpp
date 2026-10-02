#include "audio_mixer.h"

#include <dlfcn.h>
#include <pthread.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "ts_muxer.h"

static constexpr int kBlock = TsMuxer::kAudioFrame;
static constexpr double kLiveLatency = 0.12;   // HDMI 음성 지연(초): 캡처 지터 흡수 + 영상 인코딩 지연 보정

void AudioMixer::Start(TsMuxer* mux) {
  Stop();
  mux_ = mux;
  stop_ = false;
  t0_ = clk::now();
  pos_ = 0;
  live_pos_ = -1;
  std::fill(ring_.begin(), ring_.end(), 0.f);
  running_ = true;
  th_ = std::thread(&AudioMixer::Run, this);
  if (!local_dev_.empty()) {
    { std::lock_guard<std::mutex> lk(lmu_); lq_.clear(); }
    lth_ = std::thread(&AudioMixer::LocalRun, this);
  }
}

void AudioMixer::Stop() {
  stop_ = true;
  lcv_.notify_all();
  if (th_.joinable()) th_.join();
  if (lth_.joinable()) lth_.join();
  running_ = false;
}

int64_t AudioMixer::NowIdx() const {
  return (int64_t)(std::chrono::duration<double>(clk::now() - t0_).count() * kRate);
}

void AudioMixer::Mix(const float* pcm, int n, int64_t idx) {   // mu_ 보유 상태
  if (idx < pos_) {   // 이미 인코딩된 구간은 버림
    const int64_t skip = std::min<int64_t>(pos_ - idx, n);
    pcm += skip * 2;
    n -= (int)skip;
    idx += skip;
  }
  for (int i = 0; i < n; i++) {
    float* d = &ring_[((idx + i) & (kCap - 1)) * 2];
    d[0] = std::clamp(d[0] + pcm[2 * i], -1.f, 1.f);
    d[1] = std::clamp(d[1] + pcm[2 * i + 1], -1.f, 1.f);
  }
}

bool AudioMixer::PushAt(const float* pcm, int n, clk::time_point due) {
  if (!running_) return false;
  const int64_t idx = (int64_t)std::llround(std::chrono::duration<double>(due - t0_).count() * kRate);
  std::lock_guard<std::mutex> lk(mu_);
  if (idx + n > pos_ + 3 * kRate) return false;
  Mix(pcm, n, idx);
  return true;
}

void AudioMixer::PushLive(const float* pcm, int n) {
  if (!running_) return;
  std::lock_guard<std::mutex> lk(mu_);
  const int64_t target = NowIdx() + (int64_t)(kLiveLatency * kRate);
  if (live_pos_ < 0 || std::llabs(live_pos_ - target) > kRate / 10) live_pos_ = target;   // 100ms 넘게 어긋나면 재동기
  Mix(pcm, n, live_pos_);
  live_pos_ += n;
}

void AudioMixer::Clear() {
  std::lock_guard<std::mutex> lk(mu_);
  for (int64_t i = 0; i < kCap; i++) {   // 현재 위치 이후 전부 비움
    float* d = &ring_[((pos_ + i) & (kCap - 1)) * 2];
    d[0] = d[1] = 0.f;
  }
  live_pos_ = -1;
}

void AudioMixer::Run() {
  pthread_setname_np(pthread_self(), "cg-audio");
  std::vector<float> blk(kBlock * 2);
  int64_t n = 0;
  while (!stop_) {
    // 블록 n 의 끝 시각까지 기다린 뒤(샘플이 모두 도착한 시점) 인코딩
    const auto start = t0_ + std::chrono::duration_cast<clk::duration>(std::chrono::duration<double>(n * (double)kBlock / kRate));
    const auto end = start + std::chrono::duration_cast<clk::duration>(std::chrono::duration<double>((double)kBlock / kRate));
    while (!stop_ && clk::now() < end) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    if (stop_) break;
    {
      std::lock_guard<std::mutex> lk(mu_);
      for (int i = 0; i < kBlock; i++) {
        float* s = &ring_[((pos_ + i) & (kCap - 1)) * 2];
        blk[2 * i] = s[0];
        blk[2 * i + 1] = s[1];
        s[0] = s[1] = 0.f;
      }
      pos_ += kBlock;
    }
    mux_->WriteAudio(blk.data(), start);
    if (!local_dev_.empty()) {
      std::vector<int16_t> s(kBlock * 2);
      for (int i = 0; i < kBlock * 2; i++) s[i] = (int16_t)std::lrintf(std::clamp(blk[i], -1.f, 1.f) * 32767.f);
      {
        std::lock_guard<std::mutex> lk(lmu_);
        lq_.push_back(std::move(s));
        while (lq_.size() > 8) lq_.pop_front();   // 재생이 막혀도 쌓이지 않게(송출 우선)
      }
      lcv_.notify_one();
    }
    n++;
  }
}

// 로컬 재생: libasound.so.2 를 dlopen (캡처와 같은 방식). 장치가 없거나 에러면 닫고 재시도한다.
void AudioMixer::LocalRun() {
  pthread_setname_np(pthread_self(), "cg-audio-out");
  struct Api {
    int (*open)(void**, const char*, int, int);
    int (*set_params)(void*, int, int, unsigned, unsigned, int, unsigned);
    long (*writei)(void*, const void*, unsigned long);
    int (*recover)(void*, int, int);
    int (*close)(void*);
  } a{};
  void* lib = dlopen("libasound.so.2", RTLD_NOW);
  if (lib) {
    a.open = (decltype(a.open))dlsym(lib, "snd_pcm_open");
    a.set_params = (decltype(a.set_params))dlsym(lib, "snd_pcm_set_params");
    a.writei = (decltype(a.writei))dlsym(lib, "snd_pcm_writei");
    a.recover = (decltype(a.recover))dlsym(lib, "snd_pcm_recover");
    a.close = (decltype(a.close))dlsym(lib, "snd_pcm_close");
  }
  if (!lib || !a.open || !a.set_params || !a.writei || !a.recover || !a.close) {
    fprintf(stderr, "[audio] libasound.so.2 를 쓸 수 없음 - 로컬 음성 출력 없음
");
    if (lib) dlclose(lib);
    return;
  }
  // snd_pcm.h 상수: STREAM_PLAYBACK=0, FORMAT_S16_LE=2, ACCESS_RW_INTERLEAVED=3
  void* h = nullptr;
  bool warned = false;
  while (!stop_) {
    if (!h) {   // 모니터가 아직 안 붙었거나 장치가 준비 안 되면 재시도
      if (a.open(&h, local_dev_.c_str(), 0, 0) < 0 || a.set_params(h, 2, 3, 2, kRate, 1, 150000) < 0) {
        if (h) { a.close(h); h = nullptr; }
        if (!warned) { fprintf(stderr, "[audio] 로컬 출력 열기 실패 (%s) - 재시도
", local_dev_.c_str()); warned = true; }
        for (int i = 0; i < 10 && !stop_; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        continue;
      }
      warned = false;
      printf("[audio] 로컬 출력 시작: %s
", local_dev_.c_str());
      const std::vector<int16_t> silence(kBlock * 2, 0);   // 시작 직후 언더런 방지용 선행 무음 2블록
      for (int i = 0; i < 2; i++) a.writei(h, silence.data(), kBlock);
    }
    std::vector<int16_t> blk;
    {
      std::unique_lock<std::mutex> lk(lmu_);
      lcv_.wait_for(lk, std::chrono::milliseconds(100), [&] { return stop_ || !lq_.empty(); });
      if (lq_.empty()) continue;
      blk = std::move(lq_.front());
      lq_.pop_front();
    }
    long n = a.writei(h, blk.data(), kBlock);
    if (n < 0 && a.recover(h, (int)n, 1) < 0) {   // 복구 안 되면(모니터 분리 등) 닫고 다시 연다
      a.close(h);
      h = nullptr;
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
  }
  if (h) a.close(h);
  dlclose(lib);
}
