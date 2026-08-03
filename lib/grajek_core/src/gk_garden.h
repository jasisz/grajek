// Phrase memory shared by the device and the laptop rig.
//
// This is deliberately a state model, not a player: callers provide time and
// randomness, then decide how selected phrases sound. It has no Arduino,
// FreeRTOS, audio-engine or persistence dependencies.
#pragma once

#include <stdint.h>

namespace gk {

constexpr int kGardenCapacity = 32;
constexpr int kGardenPhraseMax = 6;

struct GardenPhraseNote {
  float cents = 0.0f;
  uint16_t gapMs = 0;  // onset distance from previous note; first = 0
};

struct GardenPhrase {
  GardenPhraseNote note[kGardenPhraseMax];
  int count = 0;
};

using Random01Fn = float (*)(void* context);

class Garden {
 public:
  void clear();

  // `nowMs` is supplied by the adapter so tests, Arduino and the host all use
  // the same phrase-boundary code. Unsigned subtraction is wrap-safe.
  void push(float cents, uint32_t nowMs);

  // Restored memories do not continue yesterday's unfinished phrase: the
  // next live note always begins a new one.
  void restore(const float* cents, const uint16_t* delayMs,
               uint32_t phraseStartMask, int count);

  int count() const { return count_; }
  int phraseCount() const;
  float cents(int oldestIndex) const;
  uint16_t delayMs(int oldestIndex) const;
  bool startsPhrase(int oldestIndex) const;
  float freshestCents() const;

  bool phraseAtAnchor(GardenPhrase* out, int anchor) const;
  bool selectPhrase(GardenPhrase* out, Random01Fn random01,
                    void* randomContext) const;
  bool pluck(GardenPhrase* out, float direction, Random01Fn random01,
             void* randomContext) const;

  static uint16_t replayGapMs(uint16_t recordedMs);

 private:
  struct Memory {
    float cents = 0.0f;
    uint16_t gapMs = 0;
    bool phraseStart = false;
  };

  const Memory& memoryAt(int oldestIndex) const;

  Memory ring_[kGardenCapacity]{};
  int head_ = 0;
  int count_ = 0;
  uint32_t lastOnsetMs_ = 0;
  uint32_t captureStartedMs_ = 0;
  int captureNotes_ = 0;
  bool haveLiveOnset_ = false;
};

}  // namespace gk
