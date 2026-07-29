// Exponential ADSR envelope (one-pole approach toward a target).
#pragma once
#include <math.h>

namespace ga {

class ADSR {
 public:
  void init(float sampleRate) {
    sr_ = sampleRate;
    setTimes(0.03f, 0.25f, 0.8f, 0.5f);
    reset();
  }
  void setTimes(float attackS, float decayS, float sustain, float releaseS) {
    aCoef_ = coef(attackS);
    dCoef_ = coef(decayS);
    rCoef_ = coef(releaseS);
    sus_ = sustain < 0.0f ? 0.0f : (sustain > 1.0f ? 1.0f : sustain);
  }
  void noteOn() { stage_ = Attack; }
  void noteOff() {
    if (stage_ != Idle) stage_ = Release;
  }
  void reset() {
    stage_ = Idle;
    env_ = 0.0f;
  }
  bool active() const { return stage_ != Idle; }
  bool releasing() const { return stage_ == Release; }

  float process() {
    switch (stage_) {
      case Attack:
        // target above 1.0 so the attack finishes in finite time
        env_ += aCoef_ * (1.25f - env_);
        if (env_ >= 1.0f) { env_ = 1.0f; stage_ = Decay; }
        break;
      case Decay:
        env_ += dCoef_ * (sus_ - env_);
        if (env_ <= sus_ + 1e-4f) {
          env_ = sus_;
          // sustain == 0: go straight to Idle instead of holding a silent
          // voice forever (it would waste a polyphony slot and CPU)
          stage_ = (sus_ <= 1e-4f) ? Idle : Sustain;
          if (stage_ == Idle) env_ = 0.0f;
        }
        break;
      case Sustain:
        env_ = sus_;
        break;
      case Release:
        env_ += rCoef_ * (0.0f - env_);
        if (env_ <= 1e-4f) { env_ = 0.0f; stage_ = Idle; }
        break;
      case Idle:
      default:
        break;
    }
    return env_;
  }

 private:
  enum Stage { Idle, Attack, Decay, Sustain, Release };
  float coef(float seconds) const {
    float n = seconds * sr_;
    if (n < 1.0f) n = 1.0f;
    return 1.0f - expf(-4.0f / n);  // ~98% of the way in `seconds`
  }
  float sr_ = 48000.0f;
  float env_ = 0.0f;
  float sus_ = 0.8f;
  float aCoef_ = 0.0f, dCoef_ = 0.0f, rCoef_ = 0.0f;
  Stage stage_ = Idle;
};

}  // namespace ga
