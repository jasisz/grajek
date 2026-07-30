// Engine: polyphony + global filter + parameters, driven by an event queue.
//
// Threading contract:
//  - noteOn/noteOff/allNotesOff/setParam — control thread (UI/input),
//    lock-free, never blocks.
//  - process() — audio thread only; produces mono float in [-1, 1].
// No allocation after init(). No hardware dependencies.
#pragma once
#include <stdint.h>
#include <atomic>
#include "ga_events.h"
#include "ga_voice.h"
#include "ga_svf.h"

namespace ga {

class Engine {
 public:
  static constexpr int kMaxVoices = 10;
  static constexpr int kMaxBlock = 128;

  void init(float sampleRate);

  // --- control-thread API ---
  void noteOn(int32_t id, float cents, float vel = 1.0f);
  void noteOff(int32_t id);
  void allNotesOff();
  void setParam(Param p, float v);

  // --- audio-thread API ---
  void process(float* out, int nFrames);
  static void toInt16(const float* in, int16_t* out, int n);

  int activeVoiceCount() const {
    return activeCount_.load(std::memory_order_relaxed);
  }
  float sampleRate() const { return sr_; }

 private:
  void handle(const Event& e);
  Voice* voiceForNewNote(int32_t id);
  void renderBlock(float* out, int n);

  EventQueue<256> q_;
  // Safety valve: if the queue is full when a NoteOff/AllOff arrives, dropping
  // it would leave a note stuck forever — escalate to "release everything".
  std::atomic<bool> pendingAllOff_{false};
  Voice voices_[kMaxVoices];
  SVF filter_;
  Timbre timbre_;
  float sr_ = 48000.0f;
  float masterGain_ = 0.5f, masterGainTarget_ = 0.5f;
  float baseHz_ = 220.0f;
  float glideSec_ = 0.0f;
  float bend_ = 0.0f, bendTarget_ = 0.0f;
  float cutoff_ = 7000.0f, cutoffTarget_ = 7000.0f, res_ = 0.15f;
  bool bassVoicing_ = true;  // virtual-pitch bass (Param::BassVoicing)
  float polyComp_ = 1.0f;  // equal-power polyphony compensation, smoothed
  uint32_t ageCounter_ = 0;
  std::atomic<int> activeCount_{0};
};

}  // namespace ga
