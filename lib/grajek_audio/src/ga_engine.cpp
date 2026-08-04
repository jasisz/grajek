#include "ga_engine.h"
#include <string.h>
#include <math.h>

namespace ga {

void Engine::init(float sampleRate) {
  sr_ = sampleRate;
  sineTable();  // build the table before the audio thread starts
  for (int i = 0; i < kMaxVoices; ++i)
    voices_[i].init(sampleRate, 0x9E3779B9u * (uint32_t)(i + 1));
  filter_.init(sampleRate);
  timbre_ = timbrePreset(0);
  masterGain_ = masterGainTarget_ = 0.5f;
  baseHz_ = 220.0f;
  glideSec_ = 0.0f;
  bend_ = bendTarget_ = 0.0f;
  cutoff_ = cutoffTarget_ = 7000.0f;
  res_ = resTarget_ = timbre_.filterRes;
  filterScale_ = filterScaleTarget_ = timbre_.filterScale;
  filterEnvOct_ = filterEnvOctTarget_ = timbre_.filterEnvOct;
  filterKeyMul_ = filterKeyMulTarget_ = 1.0f;
  drive_ = driveTarget_ = timbre_.drive;
  filterModeFrom_ = filterModeTarget_ = FilterMode::Lowpass;
  filterModeBlend_ = 1.0f;
  filterEnvStage_ = FilterEnvStage::Idle;
  filterEnv_ = 0.0f;
  polyComp_ = 1.0f;
  ageCounter_ = 0;
}

void Engine::noteOn(int32_t id, float cents, float vel) {
  Event e;
  e.type = Event::NoteOn;
  e.id = id;
  e.a = cents;
  e.b = vel;
  q_.push(e);
}

void Engine::noteOnWithPreset(int32_t id, float cents, float vel, int preset,
                              float attackSec) {
  Event e;
  e.type = Event::NoteOn;
  e.id = id;
  e.a = cents;
  e.b = vel;
  e.preset = (preset >= 0 && preset < 128) ? (int8_t)preset : (int8_t)-1;
  if (attackSec >= 0.0f) {
    e.c = attackSec;
    e.attackOverride = true;
  }
  q_.push(e);
}

void Engine::noteOnGlide(int32_t id, float fromCents, float cents, float vel,
                         float glideSec) {
  Event e;
  e.type = Event::NoteOn;
  e.id = id;
  e.a = cents;
  e.b = vel;
  e.c = fromCents;
  e.glideFrom = true;
  const float ms = clampf(glideSec * 1000.0f, 1.0f, 255.0f);
  e.glideMs = (uint8_t)ms;
  q_.push(e);
}

void Engine::noteOnPersistent(int32_t id, float cents, float vel) {
  Event e;
  e.type = Event::NoteOn;
  e.id = id;
  e.a = cents;
  e.b = vel;
  e.persistent = true;
  q_.push(e);
}

void Engine::noteOnPersistentWithPreset(int32_t id, float cents, float vel,
                                        int preset) {
  Event e;
  e.type = Event::NoteOn;
  e.id = id;
  e.a = cents;
  e.b = vel;
  e.preset = (preset >= 0 && preset < 128) ? (int8_t)preset : (int8_t)-1;
  e.persistent = true;
  q_.push(e);
}

void Engine::noteOff(int32_t id) {
  Event e;
  e.type = Event::NoteOff;
  e.id = id;
  if (!q_.push(e)) pendingAllOff_.store(true, std::memory_order_release);
}

void Engine::allNotesOff() {
  Event e;
  e.type = Event::AllOff;
  if (!q_.push(e)) pendingAllOff_.store(true, std::memory_order_release);
}

void Engine::setParam(Param p, float v) {
  Event e;
  e.type = Event::SetParam;
  e.param = p;
  e.a = v;
  q_.push(e);
}

Voice* Engine::voiceForNewNote(int32_t id) {
  // 1) the same note is still sounding — retrigger/legato
  for (auto& v : voices_)
    if (v.active() && v.id() == id) return &v;
  // 2) a free voice
  for (auto& v : voices_)
    if (!v.active()) return &v;
  // 3) steal: ordinary releasing voices, then ordinary active voices. A
  // persistent drone is the floor of the instrument, not the first victim
  // merely because it has been sounding longest.
  Voice* best = nullptr;
  for (auto& v : voices_)
    if (v.releasing() && !v.persistent() &&
        (!best || v.level() < best->level() ||
         (v.level() == best->level() && v.age() < best->age())))
      best = &v;
  if (best) return best;
  for (auto& v : voices_)
    if (!v.persistent() && (!best || v.age() < best->age())) best = &v;
  if (best) return best;
  // Defensive fallback: an application should never pin all ten voices.
  for (auto& v : voices_)
    if (!best || v.age() < best->age()) best = &v;
  return best;
}

void Engine::handle(const Event& e) {
  switch (e.type) {
    case Event::NoteOn: {
      Voice* v = voiceForNewNote(e.id);
      // Glide only on legato retrigger of the same note id; a stolen voice
      // must not sweep in from an unrelated pitch.
      const bool sameId = v->active() && v->id() == e.id;

      // Perceptual key tracking (equal-loudness contour): high notes land
      // quieter and darker, low notes get a little body — a lazy keypress is
      // perceptually balanced in every register, piercing highs cannot
      // happen. Piecewise-linear gain over octaves relative to 220 Hz.
      const float hz = baseHz_ * centsToRatio(e.a);
      // The grid spans more than the +/-2 octaves the loudness curve covers,
      // so key tracking needs the unclamped distance from the 220 Hz centre.
      const float octRaw = clampf(log2f(hz / 220.0f), -4.0f, 4.0f);
      const float oct = clampf(octRaw, -2.0f, 2.0f);
      // linear gains at octaves -2..+2: {+2, +2.5, 0, -3, -6} dB
      static const float kLoud[5] = {1.26f, 1.33f, 1.0f, 0.708f, 0.501f};
      const float x = oct + 2.0f;
      const int i0 = (int)x > 3 ? 3 : (int)x;
      const float frac = x - (float)i0;
      const float loud = kLoud[i0] + (kLoud[i0 + 1] - kLoud[i0]) * frac;
      // brightness tracking: upper partials fade as pitch rises
      int notePreset = (int)e.preset;
      if (notePreset < 0 || notePreset >= kNumTimbrePresets) notePreset = -1;
      Timbre tl = notePreset >= 0 ? timbrePreset(notePreset) : timbre_;
      if (e.attackOverride) tl.attack = clampf(e.c, 0.001f, 10.0f);
      const float bright = clampf(1.0f - 0.25f * oct, 0.5f, 1.3f);
      for (int k = 1; k < Timbre::kPartials; ++k) tl.gain[k] *= bright;

      // Only the physical 4x14 grid opens the paraphonic colour envelope or
      // moves its key tracking. Background weather, ghosts and the heartbeat
      // must not pump the whole room merely because their scheduler emitted a
      // note — and a ghost two octaves up must not drag the filter with it.
      if (e.id >= 0 && e.id < 56) {
        if (timbre_.filterEnvOct > 0.001f)
          filterEnvStage_ = FilterEnvStage::Attack;
        if (timbre_.filterKeyTrack > 0.001f)
          filterKeyMulTarget_ = exp2f(timbre_.filterKeyTrack * octRaw);
      }

      const float noteGlide = e.glideFrom
                                  ? (float)e.glideMs * 0.001f
                                  : (sameId ? glideSec_ : 0.0f);
      v->noteOn(e.id, e.a, clampf(e.b * loud, 0.0f, 1.0f), tl,
                noteGlide, ++ageCounter_, e.persistent,
                e.glideFrom, e.c);
      break;
    }
    case Event::NoteOff:
      for (auto& v : voices_)
        if (v.active() && v.id() == e.id) v.noteOff();
      break;
    case Event::AllOff:
      for (auto& v : voices_) v.noteOff();
      break;
    case Event::SetParam:
      switch (e.param) {
        case Param::MasterGain:     masterGainTarget_ = clampf(e.a, 0.0f, 1.0f); break;
        case Param::FilterCutoffHz: cutoffTarget_ = clampf(e.a, 40.0f, 14000.0f); break;
        case Param::FilterRes:      resTarget_ = clampf(e.a, 0.0f, 1.0f); break;
        case Param::BendCents:      bendTarget_ = clampf(e.a, -2400.0f, 2400.0f); break;
        case Param::GlideSec:       glideSec_ = clampf(e.a, 0.0f, 5.0f); break;
        case Param::BaseHz:         baseHz_ = clampf(e.a, 20.0f, 2000.0f); break;
        // Envelope params affect notes played after the change.
        case Param::EnvAttack:      timbre_.attack = clampf(e.a, 0.001f, 10.0f); break;
        case Param::EnvDecay:       timbre_.decay = clampf(e.a, 0.001f, 10.0f); break;
        case Param::EnvSustain:     timbre_.sustain = clampf(e.a, 0.0f, 1.0f); break;
        case Param::EnvRelease:     timbre_.release = clampf(e.a, 0.001f, 20.0f); break;
        case Param::TimbrePreset: {
          int idx = (int)e.a;
          if (idx < 0) idx = 0;
          if (idx >= kNumTimbrePresets) idx = kNumTimbrePresets - 1;
          const Timbre next = timbrePreset(idx);
          if (next.filterMode != filterModeTarget_) {
            // Crossfade output taps of the SAME SVF state. The remaining bus
            // parameters below have their own smooth targets as well.
            filterModeFrom_ = filterModeTarget_;
            filterModeTarget_ = next.filterMode;
            filterModeBlend_ = 0.0f;
          }
          timbre_ = next;
          resTarget_ = timbre_.filterRes;
          filterScaleTarget_ = timbre_.filterScale;
          filterEnvOctTarget_ = timbre_.filterEnvOct;
          driveTarget_ = timbre_.drive;
          if (timbre_.filterEnvOct <= 0.001f &&
              filterEnvStage_ == FilterEnvStage::Attack)
            filterEnvStage_ = FilterEnvStage::Decay;
          // A preset that does not key-track must not inherit the last note's
          // multiplier, or the legacy timbres would drift with the keyboard.
          if (timbre_.filterKeyTrack <= 0.001f) filterKeyMulTarget_ = 1.0f;
          break;
        }
        case Param::BassVoicing:    bassVoicingTarget_ = e.a > 0.5f ? 1.0f : 0.0f; break;
      }
      break;
    case Event::None:
      break;
  }
}

void Engine::renderBlock(float* out, int n) {
  float mix[kMaxBlock];
  memset(mix, 0, sizeof(float) * (size_t)n);

  constexpr int kFilterControlStride = 16;  // 3 kHz control during motion
  // Per-block parameter smoothing (no zipper noise). Every coefficient here
  // is a function of the block size, the sample rate and — for the colour
  // envelope — the preset. None of those move from one block to the next, yet
  // this was six expf calls plus a handful of divides, 333 times a second,
  // always producing the same numbers. Rebuild them only when an input moves.
  const float attackSeconds = timbre_.filterEnvAttack > 0.001f
                                  ? timbre_.filterEnvAttack
                                  : 0.001f;
  const float decaySeconds = timbre_.filterEnvDecay > 0.001f
                                 ? timbre_.filterEnvDecay
                                 : 0.001f;
  if (n != coefN_ || attackSeconds != coefAttack_ ||
      decaySeconds != coefDecay_) {
    coefN_ = n;
    coefAttack_ = attackSeconds;
    coefDecay_ = decaySeconds;
    bc_ = 1.0f - expf(-(float)n / (sr_ * 0.02f));
    cc_ = 1.0f - expf(-(float)n / (sr_ * 0.035f));
    sc_ = 1.0f - expf(-(float)n / (sr_ * 0.15f));
    busCoefStride_ =
        1.0f - expf(-(float)kFilterControlStride / (sr_ * 0.040f));
    busCoefBlock_ = 1.0f - expf(-(float)n / (sr_ * 0.040f));
    filterAttackStep_ = 1.0f / (sr_ * attackSeconds);
    filterDecayMul_ = expf(-4.6051702f / (sr_ * decaySeconds));
    modeStep_ = 1.0f / (sr_ * 0.06f);  // clickless 60 ms hand-over
  }
  const float bc = bc_;
  bend_ += bc * (bendTarget_ - bend_);
  masterGain_ += bc * (masterGainTarget_ - masterGain_);
  cutoff_ += cc_ * (cutoffTarget_ - cutoff_);

  // ~50 ms hand-over, the same idiom as the other bus parameters.
  bassVoicing_ += bc * (bassVoicingTarget_ - bassVoicing_);
  if (fabsf(bassVoicingTarget_ - bassVoicing_) < 1e-4f)
    bassVoicing_ = bassVoicingTarget_;

  const float bendRatio = centsToRatio(bend_);
  int act = 0;
  for (int i = 0; i < kMaxVoices; ++i) {
    if (voices_[i].active()) {
      voices_[i].render(mix, n, baseHz_, bendRatio, bassVoicing_);
      ++act;
    }
  }
  activeCount_.store(act, std::memory_order_relaxed);

  // Equal-power polyphony compensation: a chord must not be N times louder
  // than a single note (it would squash into the clipper and leave no room
  // to play a lead on top). Slow smoothing avoids audible pumping as voices
  // come and go; releasing voices still count, so tails stay balanced.
  const float compTarget = 1.0f / sqrtf((float)(act > 1 ? act : 1));
  polyComp_ += sc_ * (compTarget - polyComp_);

  const float outGain = masterGain_ * polyComp_;
  const float filterAttackStep = filterAttackStep_;
  const float filterDecayMul = filterDecayMul_;
  const float modeStep = modeStep_;
  const bool fastFilterControl =
      filterEnvStage_ != FilterEnvStage::Idle ||
      fabsf(resTarget_ - res_) > 1e-4f ||
      fabsf(filterScaleTarget_ - filterScale_) > 1e-4f ||
      fabsf(filterEnvOctTarget_ - filterEnvOct_) > 1e-4f ||
      fabsf(filterKeyMulTarget_ - filterKeyMul_) > 1e-4f ||
      fabsf(driveTarget_ - drive_) > 1e-4f;
  // The bus parameters are control-rate by nature: they move only when a
  // preset changes or a key arrives. Smoothing them once per coefficient
  // update instead of once per sample costs five multiply-adds per block
  // rather than five per sample, with the time constant preserved by
  // matching the coefficient to the gap between updates.
  const float busCoefStride = busCoefStride_;
  const float busCoefBlock = busCoefBlock_;
  // Nothing is mid-crossfade in the common case, and every preset's filter
  // is a lowpass, so the tap selection can leave the sample loop entirely.
  const bool simpleTap = filterModeBlend_ >= 1.0f &&
                         filterModeFrom_ == filterModeTarget_ &&
                         filterModeTarget_ == FilterMode::Lowpass;
  float driveApplied = clampf(drive_, 1.0f, 3.0f);
  float driveMakeup = 1.0f / sqrtf(driveApplied);
  filter_.flushDenormals();
  for (int i = 0; i < n; ++i) {
    // One shared colour envelope keeps the broad-spectrum presets cheap. It
    // evolves per sample; filter coefficients follow at 3 kHz while moving,
    // avoiding the two-step 4 ms attack a 96/128-frame block would create.
    switch (filterEnvStage_) {
      case FilterEnvStage::Attack:
        filterEnv_ += filterAttackStep;
        if (filterEnv_ >= 1.0f) {
          filterEnv_ = 1.0f;
          filterEnvStage_ = FilterEnvStage::Decay;
        }
        break;
      case FilterEnvStage::Decay:
        filterEnv_ *= filterDecayMul;
        if (filterEnv_ < 0.001f) {
          filterEnv_ = 0.0f;
          filterEnvStage_ = FilterEnvStage::Idle;
        }
        break;
      case FilterEnvStage::Idle:
        break;
    }

    if (i == 0 || (fastFilterControl &&
                   (i % kFilterControlStride) == 0)) {
      const float c = fastFilterControl ? busCoefStride : busCoefBlock;
      res_ += c * (resTarget_ - res_);
      filterScale_ += c * (filterScaleTarget_ - filterScale_);
      filterEnvOct_ += c * (filterEnvOctTarget_ - filterEnvOct_);
      filterKeyMul_ += c * (filterKeyMulTarget_ - filterKeyMul_);
      drive_ += c * (driveTarget_ - drive_);
      const float actualCutoff = clampf(
          cutoff_ * filterScale_ * filterKeyMul_ *
              exp2f(filterEnvOct_ * filterEnv_),
          40.0f, 14000.0f);
      filter_.set(actualCutoff, res_);
      driveApplied = clampf(drive_, 1.0f, 3.0f);
      driveMakeup = 1.0f / sqrtf(driveApplied);
    }

    const float driven = mix[i] * outGain * driveApplied;
    if (simpleTap) {
      out[i] = softClip(filter_.processLP(driven)) * driveMakeup;
      continue;
    }
    const SVFFrame f = filter_.process(driven);
    const float from = f.select(filterModeFrom_);
    const float to = f.select(filterModeTarget_);
    const float coloured = from + (to - from) * filterModeBlend_;
    out[i] = softClip(coloured) * driveMakeup;
    if (filterModeBlend_ < 1.0f) {
      filterModeBlend_ += modeStep;
      if (filterModeBlend_ >= 1.0f) {
        filterModeBlend_ = 1.0f;
        filterModeFrom_ = filterModeTarget_;
      }
    }
  }

  // Snap inaudible tails so a stable old preset returns to the one-update-
  // per-block path instead of paying the 3 kHz control rate forever.
  if (fabsf(resTarget_ - res_) < 1e-5f) res_ = resTarget_;
  if (fabsf(filterScaleTarget_ - filterScale_) < 1e-5f)
    filterScale_ = filterScaleTarget_;
  if (fabsf(filterEnvOctTarget_ - filterEnvOct_) < 1e-5f)
    filterEnvOct_ = filterEnvOctTarget_;
  if (fabsf(filterKeyMulTarget_ - filterKeyMul_) < 1e-5f)
    filterKeyMul_ = filterKeyMulTarget_;
  if (fabsf(driveTarget_ - drive_) < 1e-5f) drive_ = driveTarget_;
}

void Engine::process(float* out, int nFrames) {
  Event e;
  while (q_.pop(e)) handle(e);
  if (pendingAllOff_.exchange(false, std::memory_order_acquire))
    for (auto& v : voices_) v.noteOff();
  int done = 0;
  while (done < nFrames) {
    int n = nFrames - done;
    if (n > kMaxBlock) n = kMaxBlock;
    renderBlock(out + done, n);
    done += n;
  }
}

void Engine::toInt16(const float* in, int16_t* out, int n) {
  for (int i = 0; i < n; ++i) {
    float s = in[i];
    if (s > 1.0f) s = 1.0f;
    if (s < -1.0f) s = -1.0f;
    out[i] = (int16_t)(s * 32767.0f);
  }
}

}  // namespace ga
