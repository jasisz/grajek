#include <assert.h>
#include <math.h>
#include <stdint.h>

#include "gk_garden.h"
#include "gk_background.h"
#include "gk_lullaby.h"
#include "gk_pulse.h"

namespace {

bool near(float a, float b, float epsilon = 0.0001f) {
  return fabsf(a - b) <= epsilon;
}

struct RandomSequence {
  const float* values;
  int count;
  int index = 0;
};

float nextRandom(void* context) {
  auto* sequence = static_cast<RandomSequence*>(context);
  assert(sequence && sequence->index < sequence->count);
  return sequence->values[sequence->index++];
}

void gardenTest() {
  gk::Garden garden;
  garden.push(0.0f, 100);
  garden.push(200.0f, 220);
  garden.push(400.0f, 220);  // a chord remains inside one phrase
  garden.push(700.0f, 1720); // exact 1500 ms starts another phrase
  garden.push(900.0f, 2050);

  assert(garden.count() == 5);
  assert(garden.phraseCount() == 2);
  gk::GardenPhrase phrase;
  assert(garden.phraseAtAnchor(&phrase, 1));
  assert(phrase.count == 3);
  assert(phrase.note[1].gapMs == 120 && phrase.note[2].gapMs == 0);
  assert(garden.phraseAtAnchor(&phrase, 4) && phrase.count == 2);
  assert(gk::Garden::replayGapMs(0) == 70);
  assert(gk::Garden::replayGapMs(240) == 240);
  assert(gk::Garden::replayGapMs(1499) == 1200);

  gk::Garden bounded;
  for (int i = 0; i < 6; ++i) bounded.push((float)i, 1000 + i * 100);
  bounded.push(6.0f, 1600);
  assert(bounded.startsPhrase(6));

  gk::Garden duration;
  for (int i = 0; i < 5; ++i) duration.push((float)i, i * 1000u);
  duration.push(5.0f, 5000);
  assert(duration.startsPhrase(5));

  gk::Garden wrappedClock;
  const uint32_t start = UINT32_MAX - 1000u;
  wrappedClock.push(0.0f, start);
  wrappedClock.push(1.0f, start + 100u);
  wrappedClock.push(2.0f, start + 1600u);
  assert(!wrappedClock.startsPhrase(1));
  assert(wrappedClock.startsPhrase(2));

  gk::Garden ring;
  for (int i = 0; i <= gk::kGardenCapacity; ++i)
    ring.push((float)i, 1000u + (uint32_t)i * 100u);
  assert(ring.count() == gk::kGardenCapacity);
  assert(ring.startsPhrase(0) && ring.delayMs(0) == 0);

  const float restoredCents[] = {10.0f, 20.0f, 30.0f};
  const uint16_t restoredDelay[] = {0, 100, 0};
  gk::Garden restored;
  restored.restore(restoredCents, restoredDelay, 0b101u, 3);
  assert(restored.phraseCount() == 2);
  restored.push(40.0f, 1234);
  assert(restored.startsPhrase(3));

  const float randomValues[] = {0.0f, 0.80f};
  RandomSequence random{randomValues, 2};
  assert(garden.pluck(&phrase, 1.0f, nextRandom, &random));
  assert(phrase.count == 2);
  assert(near(phrase.note[0].cents, 1900.0f));
  assert(near(phrase.note[1].cents, 2100.0f));
}

void pulseTest() {
  gk::PulseTracker pulse;
  pulse.onOnset(1.0);
  pulse.onOnset(1.5);
  pulse.onOnset(2.0);
  assert(!pulse.ticking());
  pulse.onOnset(2.5);
  assert(pulse.ticking());
  assert(fabs(pulse.periodSec() - 0.5) < 1e-9);
  assert(!pulse.tick(2.999, 2).play);

  const gk::PulseBeat first = pulse.tick(3.0, 2);
  assert(first.play && first.chordIndex == 0);
  assert(near(first.velocity, 0.10812f));
  const gk::PulseBeat second = pulse.tick(3.5, 2);
  assert(second.play && second.chordIndex == 1);

  assert(pulse.tick(4.0, 2).play);
  assert(pulse.tick(4.5, 2).play);
  assert(pulse.tick(5.0, 2).play);
  assert(pulse.tick(5.5, 2).play);
  assert(!pulse.tick(6.0, 2).play);
  assert(!pulse.ticking());
  assert(fabs(pulse.memoryPeriodSec() - 0.5) < 1e-9);

  pulse.restoreMemory(0.75);
  assert(!pulse.ticking());
  assert(fabs(pulse.memoryPeriodSec() - 0.75) < 1e-9);
  pulse.onOnset(10.0);
  pulse.onOnset(10.5);
  assert(!pulse.suspend());
  assert(!pulse.ticking());
}

void lullabyTest() {
  gk::Garden empty;
  gk::LullabySequencer lullaby;
  assert(!lullaby.start(1000, true, empty));

  gk::Garden garden;
  garden.push(100.0f, 100);
  garden.push(300.0f, 300);
  assert(!lullaby.start(1000, false, garden));
  assert(lullaby.start(1000, true, garden));
  assert(lullaby.active());
  assert(lullaby.tick(2199, garden).type == gk::LullabyEventType::None);

  const gk::LullabyEvent first = lullaby.tick(2200, garden);
  assert(first.type == gk::LullabyEventType::PlayNote);
  assert(near(first.cents, 100.0f) && near(first.velocity, 0.18f));
  const gk::LullabyEvent second = lullaby.tick(2400, garden);
  assert(second.type == gk::LullabyEventType::PlayNote);
  assert(near(second.cents, 300.0f));
  assert(lullaby.tick(3882, garden).type == gk::LullabyEventType::None);
  assert(lullaby.tick(3883, garden).type ==
         gk::LullabyEventType::EnterSleep);
  assert(lullaby.tick(6882, garden).type == gk::LullabyEventType::None);
  assert(lullaby.tick(6883, garden).type == gk::LullabyEventType::SaveDue);
  lullaby.acknowledgeSave(false, 6883);
  assert(lullaby.tick(36882, garden).type == gk::LullabyEventType::None);
  assert(lullaby.tick(36883, garden).type == gk::LullabyEventType::SaveDue);
  lullaby.acknowledgeSave(true, 36883);
  assert(lullaby.tick(70000, garden).type == gk::LullabyEventType::None);
  lullaby.abort();
  assert(!lullaby.active());

  gk::Garden oneNote;
  oneNote.push(42.0f, 1);
  gk::LullabySequencer wrapped;
  const uint32_t nearWrap = UINT32_MAX - 500u;
  assert(wrapped.start(nearWrap, true, oneNote));
  assert(wrapped.tick(UINT32_MAX, oneNote).type ==
         gk::LullabyEventType::None);
  assert(wrapped.tick(698, oneNote).type == gk::LullabyEventType::None);
  assert(wrapped.tick(699, oneNote).type ==
         gk::LullabyEventType::PlayNote);

  // A deadline value of zero is valid after uint32 wrap; it must not double as
  // the "no save pending" sentinel.
  gk::LullabySequencer zeroDeadline;
  const uint32_t enterSleepAt = UINT32_MAX - 2999u;
  const uint32_t playAt = enterSleepAt - 1400u;
  assert(zeroDeadline.start(playAt - 1200u, true, oneNote));
  assert(zeroDeadline.tick(playAt, oneNote).type ==
         gk::LullabyEventType::PlayNote);
  assert(zeroDeadline.tick(enterSleepAt, oneNote).type ==
         gk::LullabyEventType::EnterSleep);
  assert(zeroDeadline.tick(UINT32_MAX, oneNote).type ==
         gk::LullabyEventType::None);
  assert(zeroDeadline.tick(0, oneNote).type ==
         gk::LullabyEventType::SaveDue);

  gk::LullabySequencer zeroRetry;
  const uint32_t retryDueAt = UINT32_MAX - 29999u;
  const uint32_t retrySleepAt = retryDueAt - 3000u;
  const uint32_t retryPlayAt = retrySleepAt - 1400u;
  assert(zeroRetry.start(retryPlayAt - 1200u, true, oneNote));
  assert(zeroRetry.tick(retryPlayAt, oneNote).type ==
         gk::LullabyEventType::PlayNote);
  assert(zeroRetry.tick(retrySleepAt, oneNote).type ==
         gk::LullabyEventType::EnterSleep);
  assert(zeroRetry.tick(retryDueAt, oneNote).type ==
         gk::LullabyEventType::SaveDue);
  zeroRetry.acknowledgeSave(false, retryDueAt);
  assert(zeroRetry.tick(UINT32_MAX, oneNote).type ==
         gk::LullabyEventType::None);
  assert(zeroRetry.tick(0, oneNote).type ==
         gk::LullabyEventType::SaveDue);
}

void backgroundTest() {
  static_assert(static_cast<int>(gk::BackgroundId::Off) == 0);
  static_assert(static_cast<int>(gk::BackgroundId::Root) == 1);
  static_assert(static_cast<int>(gk::BackgroundId::Drone) == 2);
  static_assert(static_cast<int>(gk::BackgroundId::Halo) == 3);
  assert(gk::backgroundPresetCount() == 4);

  const auto& off = gk::backgroundPreset(gk::BackgroundId::Off);
  const auto& root = gk::backgroundPreset(gk::BackgroundId::Root);
  const auto& drone = gk::backgroundPreset(gk::BackgroundId::Drone);
  const auto& halo = gk::backgroundPreset(gk::BackgroundId::Halo);
  assert(off.noteCount == 0 && !off.weatherMorph);
  assert(root.noteCount == 1 && near(root.cents[0], 0.0f));
  assert(drone.noteCount == 2 && near(drone.cents[1], 702.0f) &&
         drone.weatherMorph);
  assert(halo.noteCount == 3 && near(halo.cents[2], 968.8f) &&
         !halo.weatherMorph);
  assert(gk::backgroundPreset(static_cast<gk::BackgroundId>(99)).id ==
         gk::BackgroundId::Drone);
  assert(gk::backgroundPresetAt(-1).id == gk::BackgroundId::Drone);
  assert(gk::backgroundPresetAt(gk::backgroundPresetCount()).id ==
         gk::BackgroundId::Drone);
  assert(gk::backgroundPresetAt(256).id == gk::BackgroundId::Drone);

  const auto fresh = gk::decodeBackgroundSetting(false, 0, false, false);
  assert(fresh.id == gk::BackgroundId::Drone && !fresh.needsWrite);
  const auto oldOff = gk::decodeBackgroundSetting(false, 0, true, false);
  const auto oldOn = gk::decodeBackgroundSetting(false, 0, true, true);
  assert(oldOff.id == gk::BackgroundId::Off && oldOff.needsWrite);
  assert(oldOn.id == gk::BackgroundId::Drone && oldOn.needsWrite);
  const auto storedHalo = gk::decodeBackgroundSetting(
      true, static_cast<uint8_t>(gk::BackgroundId::Halo), true, false);
  assert(storedHalo.id == gk::BackgroundId::Halo && !storedHalo.needsWrite);
  const auto invalid = gk::decodeBackgroundSetting(true, 255, false, false);
  assert(invalid.id == gk::BackgroundId::Drone && invalid.needsWrite);
}

}  // namespace

int main() {
  gardenTest();
  pulseTest();
  lullabyTest();
  backgroundTest();
}
