#include "audio_mixer.h"

#include <pthread.h>

#include <algorithm>
#include <cmath>
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
}

void AudioMixer::Stop() {
  stop_ = true;
  if (th_.joinable()) th_.join();
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
    n++;
  }
}
