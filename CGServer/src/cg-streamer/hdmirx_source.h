#pragma once
// HDMI 입력(rk_hdmirx, V4L2 멀티플레인) -> 최신 프레임(dmabuf) 보관
// 버퍼를 VIDIOC_EXPBUF 로 dmabuf fd 로 내보내 RGA 가 복사 없이 읽는다.
// 신호 없음/해상도 변경(V4L2_EVENT_SOURCE_CHANGE) 시 자동으로 다시 연결.
#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include "video_source.h"   // VideoFrameRef

class AudioMixer;

class HdmiRxSource {
 public:
  ~HdmiRxSource() { Stop(); }
  bool Start(const std::string& dev = "/dev/video20");
  void Stop();
  bool Active() const { return running_; }
  bool HasSignal() const { return signal_; }
  // HDMI 입력 음성(ALSA 캡처 카드 rockchiphdmiin)을 보낼 믹서. Start() 전에 지정. 없으면 무음.
  void SetAudio(AudioMixer* a) { audio_ = a; }

  // 합성 스레드: 현재 프레임 사용 시작/끝 (그 사이에는 버퍼를 드라이버에 돌려주지 않음)
  bool Acquire(VideoFrameRef& out);
  void Release();

 private:
  void Run(std::string dev);
  void AudioRun();
  bool Session(const std::string& dev);   // 신호 1회 연결 ~ 끊김/변경까지

  AudioMixer* audio_ = nullptr;
  std::thread th_, ath_;
  std::atomic<bool> running_{false}, stop_{false}, signal_{false};
  std::mutex mu_;
  int64_t last_pts_us_ = 0;   // 직전 프레임 PTS(µs). 재연결해도 유지해 단조 증가를 보장
  int cur_ = -1;             // 현재 보관 중인 버퍼 index (-1 = 없음)
  VideoFrameRef cur_ref_;
};
