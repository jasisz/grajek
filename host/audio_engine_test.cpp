#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "ga_engine.h"
#include "ga_svf.h"
#include "ga_voice.h"

namespace {

constexpr int kSampleRate = 48000;
constexpr int kBlock = 128;

void assertFiniteBlock(const float* samples, int count, float* peak) {
  for (int i = 0; i < count; ++i) {
    assert(isfinite(samples[i]));
    assert(fabsf(samples[i]) <= 1.001f);
    if (fabsf(samples[i]) > *peak) *peak = fabsf(samples[i]);
  }
}

void testEveryPresetSoundsAndReleases() {
  for (int preset = 0; preset < ga::kNumTimbrePresets; ++preset) {
    ga::Engine engine;
    engine.init((float)kSampleRate);
    engine.setParam(ga::Param::TimbrePreset, (float)preset);
    engine.setParam(ga::Param::FilterCutoffHz, 7500.0f);
    engine.noteOn(7, 0.0f, 0.85f);

    float peak = 0.0f;
    float block[kBlock];
    for (int frame = 0; frame < kSampleRate; frame += kBlock) {
      engine.process(block, kBlock);
      assertFiniteBlock(block, kBlock, &peak);
    }
    assert(peak > 0.002f);

    engine.noteOff(7);
    // Long enough for the slowest existing preset (CHIME/ORGAN) to cross
    // the ADSR idle threshold. A stuck release must fail deterministically.
    for (int frame = 0; frame < 12 * kSampleRate; frame += kBlock) {
      engine.process(block, kBlock);
      assertFiniteBlock(block, kBlock, &peak);
    }
    assert(engine.activeVoiceCount() == 0);
  }
}

void testPresetRegistryAndFilterModes() {
  assert(ga::kNumTimbrePresets == 7);
  assert(ga::timbrePreset(0).oscillator == ga::OscillatorKind::Additive);
  assert(ga::timbrePreset(5).oscillator == ga::OscillatorKind::FatSaw);
  assert(ga::timbrePreset(6).oscillator == ga::OscillatorKind::Pulse);
  assert(ga::timbrePreset(6).filterMode == ga::FilterMode::Lowpass);
  // The additive timbres darken themselves through gain[]; the saw/pulse
  // paths never read gain[], so key tracking is the only thing keeping them
  // civil across the keyboard. Legacy presets must stay untracked.
  for (int preset = 0; preset <= 4; ++preset)
    assert(ga::timbrePreset(preset).filterKeyTrack == 0.0f);
  assert(ga::timbrePreset(5).filterKeyTrack > 0.0f);
  assert(ga::timbrePreset(6).filterKeyTrack > 0.0f);

  ga::SVF filter;
  filter.init((float)kSampleRate);
  filter.set(1600.0f, 0.35f);
  float energy[4] = {};
  for (int i = 0; i < 4096; ++i) {
    const float input = sinf(ga::kTau * 1100.0f * (float)i / kSampleRate);
    const ga::SVFFrame frame = filter.process(input);
    const float taps[4] = {frame.low, frame.high, frame.band, frame.notch};
    for (int tap = 0; tap < 4; ++tap) {
      assert(isfinite(taps[tap]));
      energy[tap] += taps[tap] * taps[tap];
    }
  }
  for (int tap = 0; tap < 4; ++tap) assert(energy[tap] > 0.001f);
  assert(fabsf(energy[0] - energy[1]) > 0.01f);
  assert(fabsf(energy[2] - energy[3]) > 0.01f);
}

// RMS of the settled tone plus a pitch-normalised brightness index: the RMS of
// the first difference, divided by what a pure sine at f0 would produce. So
// 1.0 reads "sine", and larger means more harmonic ladder standing above f0.
void measureSustain(int preset, float cents, float* rms, float* brightness) {
  ga::Engine engine;
  engine.init((float)kSampleRate);
  engine.setParam(ga::Param::TimbrePreset, (float)preset);
  engine.setParam(ga::Param::FilterCutoffHz, 7500.0f);
  engine.setParam(ga::Param::BaseHz, 220.0f);
  float block[kBlock];
  for (int frame = 0; frame < kSampleRate / 2; frame += kBlock)
    engine.process(block, kBlock);

  engine.noteOn(7, cents, 0.9f);
  double sumSq = 0.0, sumDiffSq = 0.0;
  long counted = 0;
  float prev = 0.0f;
  int index = 0;
  const int settle = kSampleRate / 2;  // judge the tone, not the attack
  for (int frame = 0; frame < 6 * kSampleRate / 5; frame += kBlock) {
    engine.process(block, kBlock);
    for (int i = 0; i < kBlock; ++i, ++index) {
      if (index >= settle) {
        sumSq += (double)block[i] * block[i];
        sumDiffSq += (double)(block[i] - prev) * (block[i] - prev);
        ++counted;
      }
      prev = block[i];
    }
  }
  *rms = counted ? (float)sqrt(sumSq / (double)counted) : 0.0f;
  const float f0 = 220.0f * exp2f(cents / 1200.0f);
  const float sineDiff = *rms * ga::kTau * f0 / (float)kSampleRate;
  *brightness = sineDiff > 0.0f
                    ? (float)sqrt(sumDiffSq / (double)counted) / sineDiff
                    : 0.0f;
}

// Guards the two defects that made the broad timbres unpleasant on hardware.
// A fixed bus cutoff cannot serve a keyboard: WARM's high notes fell 15 dB
// because the filter sat far below them, and a notch cannot tame a pulse, so
// HOLLOW's brightness index hit 4.7 and rasped. Key tracking fixes both.
void testEveryPresetStaysPlayableAcrossTheKeyboard() {
  for (int preset = 0; preset < ga::kNumTimbrePresets; ++preset) {
    float rmsLow = 0.0f, brightLow = 0.0f, rmsHigh = 0.0f, brightHigh = 0.0f;
    measureSustain(preset, 0.0f, &rmsLow, &brightLow);
    measureSustain(preset, 3600.0f, &rmsHigh, &brightHigh);  // top of the grid
    assert(rmsLow > 0.0005f && rmsHigh > 0.0005f);
    // Equal-loudness tracking deliberately takes ~6 dB off the top octaves.
    // Much beyond that means the filter ate the note instead of shaping it.
    assert(20.0f * log10f(rmsHigh / rmsLow) > -10.0f);
    assert(brightLow < 2.6f);
    assert(brightHigh < 2.6f);
  }
}

// The trim table is a claim about loudness, and nothing else here can see it:
// the amplitude bound above passes at any trim, and every other test compares
// a preset against ITSELF. Perceptual balance is a property of the whole chain
// (strings and reverb widen it, and it must be A-weighted), so it is measured
// off-line rather than asserted here. What this DOES lock is the structural
// rule that made an earlier revision distort: the table may only attenuate.
// Balancing by lifting the quiet presets instead put the cleanest timbre alone
// into the engine's soft clipper and made the PURE-voiced lullaby 5 dB louder.
void testTrimTableOnlyAttenuates() {
  float loudest = 0.0f;
  for (int preset = 0; preset < ga::kNumTimbrePresets; ++preset) {
    const float trim = ga::timbrePreset(preset).outputTrim;
    assert(trim > 0.0f);
    assert(trim <= 1.0f);
    if (trim > loudest) loudest = trim;
  }
  // Normalised against the loudest timbre, so the table costs headroom nowhere.
  assert(fabsf(loudest - 1.0f) < 1e-6f);
}

void testExplicitGlideIsAShortLanding() {
  ga::Voice voice;
  voice.init((float)kSampleRate, 1234u);
  voice.noteOn(1, 500.0f, 0.8f, ga::timbrePreset(5), 0.045f, 1, false,
               true, -200.0f);
  assert(fabsf(voice.currentCents() + 200.0f) < 0.001f);
  float block[kBlock] = {};
  voice.render(block, kBlock, 220.0f, 1.0f, true);
  assert(voice.currentCents() > -200.0f);
  assert(voice.currentCents() < 500.0f);
  for (int i = 0; i < 30; ++i) {
    for (float& sample : block) sample = 0.0f;
    voice.render(block, kBlock, 220.0f, 1.0f, true);
  }
  assert(voice.currentCents() > 499.0f);
}

void testPulsePwmDoesNotCarryDc() {
  ga::Voice voice;
  voice.init((float)kSampleRate, 5678u);
  voice.noteOn(2, 0.0f, 0.8f, ga::timbrePreset(6), 0.0f, 1);
  float block[kBlock];
  // Reach sustain first; the mean below should describe the oscillator, not
  // the rising envelope.
  for (int frame = 0; frame < kSampleRate; frame += kBlock) {
    for (float& sample : block) sample = 0.0f;
    voice.render(block, kBlock, 220.0f, 1.0f, false);
  }
  double sum = 0.0;
  int count = 0;
  for (int frame = 0; frame < 2 * kSampleRate; frame += kBlock) {
    for (float& sample : block) sample = 0.0f;
    voice.render(block, kBlock, 220.0f, 1.0f, false);
    for (float sample : block) {
      assert(isfinite(sample));
      sum += sample;
      ++count;
    }
  }
  assert(fabs(sum / (double)count) < 0.01);
}

void testQueueOverflowCannotLeaveHeldNote() {
  ga::Engine engine;
  engine.init((float)kSampleRate);
  engine.noteOn(4, 0.0f, 0.8f);
  float block[kBlock];
  engine.process(block, kBlock);
  assert(engine.activeVoiceCount() == 1);

  // Saturate the control queue, then make a critical NoteOff miss it. The
  // engine's pending-all-off safety valve must still release the voice.
  for (int i = 0; i < 300; ++i)
    engine.setParam(ga::Param::MasterGain, 0.5f);
  engine.noteOff(4);
  for (int frame = 0; frame < 3 * kSampleRate; frame += kBlock)
    engine.process(block, kBlock);
  assert(engine.activeVoiceCount() == 0);
}

void testPerNotePresetDoesNotReplacePlayerTimbre() {
  ga::Engine engine;
  engine.init((float)kSampleRate);
  engine.setParam(ga::Param::TimbrePreset, 5.0f);  // sustaining WARM player
  float block[kBlock];
  engine.process(block, kBlock);

  // The heartbeat/lullaby path may ask for MUSICBOX and a custom attack, but
  // that choice belongs only to this event. Let the silent test voice finish.
  engine.noteOnWithPreset(1000, 0.0f, 0.0f, 4, 0.12f);
  for (int frame = 0; frame < 3 * kSampleRate; frame += kBlock)
    engine.process(block, kBlock);
  assert(engine.activeVoiceCount() == 0);

  // A following ordinary key must still inherit WARM's non-zero sustain. If
  // the per-note event leaked globally to MUSICBOX, it would free itself.
  engine.noteOn(7, 0.0f, 0.8f);
  for (int frame = 0; frame < 3 * kSampleRate; frame += kBlock)
    engine.process(block, kBlock);
  assert(engine.activeVoiceCount() == 1);
  engine.noteOff(7);
}

}  // namespace

int main() {
  testPresetRegistryAndFilterModes();
  testEveryPresetSoundsAndReleases();
  testEveryPresetStaysPlayableAcrossTheKeyboard();
  testTrimTableOnlyAttenuates();
  testExplicitGlideIsAShortLanding();
  testPulsePwmDoesNotCarryDc();
  testQueueOverflowCannotLeaveHeldNote();
  testPerNotePresetDoesNotReplacePlayerTimbre();
  puts("audio_engine_test: ok");
}
