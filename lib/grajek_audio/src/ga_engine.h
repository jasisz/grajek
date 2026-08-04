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
  // Polyphony is a CPU budget, not a taste: on the Cardputer one block is
  // 2000 us, the effect chain claims ~700 of it before a note sounds, and a
  // voice costs ~100 us. Ten voices overran the audio task, which starved the
  // idle task until the watchdog rebooted the box mid-play. Eight leaves real
  // headroom, and the background layer never holds more than four.
  static constexpr int kMaxVoices = 8;
  static constexpr int kMaxBlock = 128;

  void init(float sampleRate);

  // --- control-thread API ---
  void noteOn(int32_t id, float cents, float vel = 1.0f);
  // Give one ambient note its own voice preset without touching the global
  // player filter/drive/chorus character. attackSec < 0 uses the preset.
  void noteOnWithPreset(int32_t id, float cents, float vel, int preset,
                        float attackSec = -1.0f);
  // A fresh polyphonic voice that begins at `fromCents` and slides into
  // pitch.  Unlike a monophonic string lane, the preceding key keeps its own
  // voice and same-row chords remain possible.  glideSec is clamped to
  // 1..255 ms: a short landing barely bends, a long one properly sings.
  void noteOnGlide(int32_t id, float fromCents, float cents, float vel = 1.0f,
                   float glideSec = 0.045f);
  // Long-lived ambience is protected from ordinary voice stealing. It can
  // still be retriggered/off'd by id and is only stolen if every voice is
  // persistent (an app-level configuration error).
  void noteOnPersistent(int32_t id, float cents, float vel = 1.0f);
  void noteOnPersistentWithPreset(int32_t id, float cents, float vel,
                                  int preset);
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
  float cutoff_ = 7000.0f, cutoffTarget_ = 7000.0f;
  float res_ = 0.15f, resTarget_ = 0.15f;
  float filterScale_ = 1.0f, filterScaleTarget_ = 1.0f;
  float filterEnvOct_ = 0.0f, filterEnvOctTarget_ = 0.0f;
  // Cutoff multiplier from the last key the player actually pressed. One bus
  // filter serves every voice, so the newest note is the one it follows.
  float filterKeyMul_ = 1.0f, filterKeyMulTarget_ = 1.0f;
  float drive_ = 1.0f, driveTarget_ = 1.0f;
  FilterMode filterModeFrom_ = FilterMode::Lowpass;
  FilterMode filterModeTarget_ = FilterMode::Lowpass;
  float filterModeBlend_ = 1.0f;
  enum class FilterEnvStage : uint8_t { Idle, Attack, Decay };
  FilterEnvStage filterEnvStage_ = FilterEnvStage::Idle;
  float filterEnv_ = 0.0f;
  // Virtual-pitch bass (Param::BassVoicing) as a ramp, not a switch: it
  // redistributes the harmonics of every sounding low note, so flipping it in
  // one sample steps the timbre of a held drone.
  float bassVoicing_ = 1.0f, bassVoicingTarget_ = 1.0f;
  // Block-rate coefficients, rebuilt only when n, sr_ or the preset's colour
  // envelope changes — see the note at the top of renderBlock.
  int coefN_ = 0;
  float coefAttack_ = 0.0f, coefDecay_ = 0.0f;
  float bc_ = 0.0f, cc_ = 0.0f, sc_ = 0.0f;
  float busCoefStride_ = 0.0f, busCoefBlock_ = 0.0f;
  float filterAttackStep_ = 0.0f, filterDecayMul_ = 0.0f, modeStep_ = 0.0f;
  float polyComp_ = 1.0f;  // equal-power polyphony compensation, smoothed
  uint32_t ageCounter_ = 0;
  std::atomic<int> activeCount_{0};
};

}  // namespace ga
