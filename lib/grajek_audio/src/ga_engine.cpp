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
  res_ = 0.15f;
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
  // 3) steal: oldest releasing voice, otherwise oldest overall
  Voice* best = nullptr;
  for (auto& v : voices_)
    if (v.releasing() && (!best || v.age() < best->age())) best = &v;
  if (best) return best;
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
      v->noteOn(e.id, e.a, e.b, timbre_, sameId ? glideSec_ : 0.0f,
                ++ageCounter_);
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
        case Param::FilterRes:      res_ = clampf(e.a, 0.0f, 1.0f); break;
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
          timbre_ = timbrePreset(idx);
          break;
        }
      }
      break;
    case Event::None:
      break;
  }
}

void Engine::renderBlock(float* out, int n) {
  float mix[kMaxBlock];
  memset(mix, 0, sizeof(float) * (size_t)n);

  // per-block parameter smoothing (no zipper noise)
  const float bc = 1.0f - expf(-(float)n / (sr_ * 0.02f));
  bend_ += bc * (bendTarget_ - bend_);
  masterGain_ += bc * (masterGainTarget_ - masterGain_);
  const float cc = 1.0f - expf(-(float)n / (sr_ * 0.035f));
  cutoff_ += cc * (cutoffTarget_ - cutoff_);
  filter_.set(cutoff_, res_);
  filter_.flushDenormals();

  const float bendRatio = centsToRatio(bend_);
  int act = 0;
  for (int i = 0; i < kMaxVoices; ++i) {
    if (voices_[i].active()) {
      voices_[i].render(mix, n, baseHz_, bendRatio);
      ++act;
    }
  }
  activeCount_.store(act, std::memory_order_relaxed);

  // Equal-power polyphony compensation: a chord must not be N times louder
  // than a single note (it would squash into the clipper and leave no room
  // to play a lead on top). Slow smoothing avoids audible pumping as voices
  // come and go; releasing voices still count, so tails stay balanced.
  const float compTarget = 1.0f / sqrtf((float)(act > 1 ? act : 1));
  const float sc = 1.0f - expf(-(float)n / (sr_ * 0.15f));
  polyComp_ += sc * (compTarget - polyComp_);

  const float outGain = masterGain_ * polyComp_;
  for (int i = 0; i < n; ++i)
    out[i] = softClip(filter_.processLP(mix[i] * outGain));
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
