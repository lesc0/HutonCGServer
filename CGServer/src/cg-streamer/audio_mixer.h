#pragma once
// 음성 믹서: 영상 파일/HDMI RX 가 넣어 주는 PCM(48kHz 스테레오 float)을 실시간 시간축에 배치하고,
// 1024 샘플마다 AAC 로 인코딩해 TsMuxer 에 쓴다. 입력이 없으면 무음을 보낸다(PMT 에 음성 스트림이 항상 있으므로).
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class TsMuxer;

class AudioMixer {
 public:
  using clk = std::chrono::steady_clock;
  static constexpr int kRate = 48000;
  ~AudioMixer() { Stop(); }
  void Start(TsMuxer* mux);
  void Stop();

  // pcm(스테레오 인터리브 n 샘플/채널)을 시각 due 에 재생되도록 배치. 이미 지난 앞부분은 버린다.
  // 너무 먼 미래(>3초)면 false.
  bool PushAt(const float* pcm, int n, clk::time_point due);
  // 라이브 입력(HDMI): 약간의 지연(kLiveLatency)을 두고 이어 붙인다. 어긋나면 재동기.
  void PushLive(const float* pcm, int n);
  void Clear();   // 아직 재생되지 않은 음성 폐기 (영상 정지/교체 시)

  // 로컬 재생: 송출(AAC)과 같은 믹스를 이 ALSA 장치로도 내보낸다(예: "plughw:CARD=rockchiphdmi1,DEV=0"). 비면 안 함. Start() 전에 지정.
  void SetLocalOut(const std::string& dev) { local_dev_ = dev; }

 private:
  void Run();
  void LocalRun();   // local_dev_ 로 재생하는 스레드 (블록 큐 -> snd_pcm_writei)
  int64_t NowIdx() const;   // 믹서 시간축의 현재 샘플 위치
  void Mix(const float* pcm, int n, int64_t idx);

  static constexpr int kCap = 1 << 18;   // 링 크기(샘플/채널) ≈ 5.5초
  std::thread th_;
  std::atomic<bool> stop_{false}, running_{false};
  TsMuxer* mux_ = nullptr;
  clk::time_point t0_;
  std::mutex mu_;
  std::vector<float> ring_ = std::vector<float>(kCap * 2, 0.f);
  int64_t pos_ = 0;        // 다음에 인코딩할 샘플 위치
  int64_t live_pos_ = -1;

  std::string local_dev_;
  std::thread lth_;
  std::mutex lmu_;
  std::condition_variable lcv_;
  std::deque<std::vector<int16_t>> lq_;   // 재생 대기 블록(S16 스테레오 인터리브)
};
