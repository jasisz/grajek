#include "gk_garden.h"

namespace gk {
namespace {

constexpr uint32_t kPhraseBreakMs = 1500;
constexpr uint32_t kPhraseMaxDurationMs = 5000;

float nextRandom(Random01Fn fn, void* context) {
  if (!fn) return 0.0f;
  const float value = fn(context);
  if (value <= 0.0f) return 0.0f;
  if (value >= 1.0f) return 0.99999994f;
  return value;
}

}  // namespace

void Garden::clear() { *this = Garden{}; }

void Garden::push(float noteCents, uint32_t nowMs) {
  const uint32_t gap = haveLiveOnset_ ? nowMs - lastOnsetMs_ : 0;
  const bool starts =
      !haveLiveOnset_ || (!holding() && nowMs - lastActivityMs_ >= kPhraseBreakMs) ||
      captureNotes_ >= kGardenPhraseMax ||
      nowMs - captureStartedMs_ >= kPhraseMaxDurationMs;
  const uint16_t storedGap = starts ? 0 : static_cast<uint16_t>(gap);

  ring_[head_] = {noteCents, storedGap, starts};
  head_ = (head_ + 1) % kGardenCapacity;
  if (count_ < kGardenCapacity) {
    ++count_;
  } else {
    // The new logical oldest can be the middle of an overwritten phrase.
    ring_[head_].phraseStart = true;
    ring_[head_].gapMs = 0;
  }

  if (starts) {
    captureStartedMs_ = nowMs;
    captureNotes_ = 1;
  } else {
    ++captureNotes_;
  }
  lastOnsetMs_ = nowMs;
  lastActivityMs_ = nowMs;
  haveLiveOnset_ = true;
}

void Garden::noteOn(int32_t id, float noteCents, uint32_t nowMs,
                    uint8_t noteVelocity) {
  if (id < 0) return;
  noteOff(id, nowMs);  // retrigger closes only this key's preceding capture
  push(noteCents, nowMs);
  Memory& note = ring_[(head_ + kGardenCapacity - 1) % kGardenCapacity];
  note.keyId = id;
  note.onsetMs = nowMs;
  note.velocity = noteVelocity > 100 ? 100 : (noteVelocity == 0 ? 1 : noteVelocity);
}

void Garden::noteOff(int32_t id, uint32_t nowMs) {
  if (id < 0) return;
  for (auto& note : ring_) {
    if (note.keyId != id) continue;
    note.holdMs = replayHoldMs(nowMs - note.onsetMs);
    note.keyId = -1;
    lastActivityMs_ = nowMs;
  }
}

void Garden::releaseAll(uint32_t nowMs) {
  for (auto& note : ring_)
    if (note.keyId >= 0) noteOff(note.keyId, nowMs);
  haveLiveOnset_ = false;
}

bool Garden::holding() const {
  for (const auto& note : ring_)
    if (note.keyId >= 0) return true;
  return false;
}

void Garden::restore(const float* restoredCents, const uint16_t* restoredDelay,
                     uint32_t phraseStartMask, int restoredCount,
                     const uint16_t* restoredHold, const uint8_t* restoredVelocity) {
  clear();
  if (restoredCount < 0) restoredCount = 0;
  if (restoredCount > kGardenCapacity) restoredCount = kGardenCapacity;
  if (restoredCount > 0 && (!restoredCents || !restoredDelay)) return;

  for (int i = 0; i < restoredCount; ++i) {
    const bool starts = i == 0 || (phraseStartMask & (1u << i));
    const uint16_t gap = restoredDelay[i] > 5000 ? 5000 : restoredDelay[i];
    ring_[i] = {restoredCents[i],
                starts ? static_cast<uint16_t>(0) : gap, starts};
    if (restoredHold) ring_[i].holdMs = replayHoldMs(restoredHold[i]);
    if (restoredVelocity)
      ring_[i].velocity = restoredVelocity[i] > 100 ? 100 :
                          (restoredVelocity[i] == 0 ? 1 : restoredVelocity[i]);
  }
  head_ = restoredCount % kGardenCapacity;
  count_ = restoredCount;
}

const Garden::Memory& Garden::memoryAt(int oldestIndex) const {
  const int index =
      (head_ - count_ + oldestIndex + 2 * kGardenCapacity) % kGardenCapacity;
  return ring_[index];
}

int Garden::phraseCount() const {
  int phrases = 0;
  for (int i = 0; i < count_; ++i)
    if (i == 0 || memoryAt(i).phraseStart) ++phrases;
  return phrases;
}

float Garden::cents(int oldestIndex) const {
  if (oldestIndex < 0 || oldestIndex >= count_) return 0.0f;
  return memoryAt(oldestIndex).cents;
}

uint16_t Garden::delayMs(int oldestIndex) const {
  if (oldestIndex < 0 || oldestIndex >= count_) return 0;
  return memoryAt(oldestIndex).gapMs;
}

uint16_t Garden::holdMs(int oldestIndex) const {
  return oldestIndex >= 0 && oldestIndex < count_ ? memoryAt(oldestIndex).holdMs : 420;
}

uint8_t Garden::velocity(int oldestIndex) const {
  return oldestIndex >= 0 && oldestIndex < count_ ? memoryAt(oldestIndex).velocity : 90;
}

bool Garden::startsPhrase(int oldestIndex) const {
  if (oldestIndex < 0 || oldestIndex >= count_) return false;
  return oldestIndex == 0 || memoryAt(oldestIndex).phraseStart;
}

float Garden::freshestCents() const {
  return count_ > 0 ? memoryAt(count_ - 1).cents : 0.0f;
}

uint16_t Garden::replayGapMs(uint16_t recordedMs) {
  // One keyboard scan is one chord, including small I2C/loop timing skew.
  if (recordedMs < 24) return 0;
  if (recordedMs > 5000) return 5000;
  return recordedMs;
}

uint16_t Garden::replayHoldMs(uint32_t recordedMs) {
  if (recordedMs < 20) return 20;
  if (recordedMs > 5000) return 5000;
  return static_cast<uint16_t>(recordedMs);
}

bool Garden::phraseAtAnchor(GardenPhrase* out, int anchor) const {
  if (!out || anchor < 0 || anchor >= count_) return false;
  int first = anchor;
  while (first > 0 && !memoryAt(first).phraseStart) --first;
  int last = anchor;
  while (last + 1 < count_ && !memoryAt(last + 1).phraseStart) ++last;

  int window = first;
  const int available = last - first + 1;
  if (available > kGardenPhraseMax) {
    window = anchor - kGardenPhraseMax / 2;
    if (window < first) window = first;
    const int latest = last - kGardenPhraseMax + 1;
    if (window > latest) window = latest;
  }
  const int noteCount =
      available < kGardenPhraseMax ? available : kGardenPhraseMax;
  out->count = noteCount;
  for (int i = 0; i < noteCount; ++i) {
    const Memory& memory = memoryAt(window + i);
    out->note[i] = {memory.cents,
                    i == 0 ? static_cast<uint16_t>(0) : memory.gapMs,
                    memory.holdMs, memory.velocity};
  }
  return noteCount > 0;
}

bool Garden::selectPhrase(GardenPhrase* out, Random01Fn random01,
                          void* randomContext) const {
  if (!out || count_ == 0) return false;
  GardenPhrase fallback;
  for (int attempt = 0; attempt < 5; ++attempt) {
    const float value = nextRandom(random01, randomContext);
    int back = static_cast<int>(value * value * static_cast<float>(count_));
    if (back >= count_) back = count_ - 1;
    const int anchor = count_ - 1 - back;
    GardenPhrase candidate;
    if (!phraseAtAnchor(&candidate, anchor)) continue;
    if (fallback.count == 0) fallback = candidate;
    if (candidate.count >= 2) {
      *out = candidate;
      return true;
    }
  }
  *out = fallback;
  return out->count > 0;
}

bool Garden::latestPhrase(GardenPhrase* out) const {
  return phraseAtAnchor(out, count_ - 1);
}

}  // namespace gk
