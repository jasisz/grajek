#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "gk_garden.h"
#include "gk_phrase_player.h"
#include "gk_soul.h"

namespace {

void captureAndRecall() {
  gk::Garden garden;
  gk::GardenPhrase phrase;
  assert(!garden.latestPhrase(&phrase));
  // A short chord tone, a held chord tone and a detached melody note.
  garden.noteOn(1, 0, 1000, 60);
  garden.noteOn(2, 700, 1004, 90);
  garden.noteOff(1, 1080);
  garden.noteOn(3, 400, 1304, 75);
  garden.noteOff(3, 1444);
  garden.noteOff(2, 1904);
  assert(!garden.holding());
  assert(garden.latestPhrase(&phrase) && phrase.count == 3);
  assert(phrase.note[0].holdMs == 80 && phrase.note[0].velocity == 60);
  assert(phrase.note[1].holdMs == 900 && phrase.note[1].gapMs == 4);
  assert(phrase.note[2].holdMs == 140 && phrase.note[2].gapMs == 300);

  gk::PhrasePlayer player;
  assert(player.start(phrase, 5000));
  auto batch = player.tick(5000);
  assert(batch.count == 2);  // both chord tones in the same audio queue batch
  assert(batch.events[0].down && batch.events[0].slot == 0);
  assert(batch.events[1].down && batch.events[1].slot == 1);
  assert(batch.events[0].note.velocity == 60);
  assert(player.tick(5079).count == 0);
  batch = player.tick(5080);
  assert(batch.count == 1 && !batch.events[0].down && batch.events[0].slot == 0);
  batch = player.tick(5300);
  assert(batch.count == 1 && batch.events[0].down && batch.events[0].slot == 2);
  batch = player.tick(5440);
  assert(batch.count == 1 && !batch.events[0].down && batch.events[0].slot == 2);
  assert(player.active());  // last onset has passed, but the chord is still held
  assert(!player.start(phrase, 5440));
  batch = player.tick(5900);
  assert(batch.count == 1 && !batch.events[0].down && batch.events[0].slot == 1);
  assert(!player.active());
  for (int repeat = 0; repeat < 5; ++repeat) {
    gk::GardenPhrase recalled;
    assert(garden.latestPhrase(&recalled));
    assert(recalled.count == 3 && recalled.note[1].cents == 700);
    assert(player.start(recalled, 7000));
    assert(player.tick(7000).count == 2);
    batch = player.cancel();
    assert(batch.count == 2 && !player.active());
    assert(player.tick(10000).count == 0);  // cancellation leaves no late notes
  }
  garden.noteOn(4, 1200, 4000);
  garden.noteOff(4, 4200);
  assert(garden.latestPhrase(&phrase) && phrase.count == 1);
  assert(phrase.note[0].cents == 1200 && phrase.note[0].holdMs == 200);
}

void clocksAndBounds() {
  gk::Garden garden;
  const uint32_t start = UINT32_MAX - 100;
  garden.noteOn(1, 100, start);
  garden.noteOff(1, start + 250);
  assert(garden.holdMs(0) == 250);
  // Sustaining is not silence; the following note continues this phrase.
  garden.clear();
  garden.noteOn(1, 100, 100);
  garden.noteOff(1, 2200);
  garden.noteOn(2, 200, 2400);
  assert(garden.phraseCount() == 1 && garden.delayMs(1) == 2300);
  garden.releaseAll(2600);
  assert(garden.holdMs(1) == 200 && !garden.holding());
  garden.noteOn(3, 300, 2650);
  assert(garden.startsPhrase(2));  // mode changes close the old phrase
  garden.noteOff(3, 100000);
  assert(garden.holdMs(2) == 5000);
  garden.noteOn(4, 400, 100100);
  garden.noteOff(4, 100100);
  assert(garden.holdMs(3) == 20);

  garden.clear();
  garden.noteOn(1, 100, 0);
  garden.noteOn(1, 200, 100);
  garden.noteOff(1, 400);
  assert(garden.holdMs(0) == 100 && garden.holdMs(1) == 300);
  for (int i = 0; i < 40; ++i) garden.noteOn(i + 10, (float)i, 500 + i * 100);
  garden.noteOff(1, 6000);  // old key may no longer touch an overwritten slot
  assert(garden.holdMs(garden.count() - 1) == 420);
  garden.releaseAll(7000);
  assert(!garden.holding());

  gk::GardenPhrase phrase;
  phrase.count = 3;
  phrase.note[0] = {100, 0, 100, 90};
  phrase.note[1] = {200, 200, 300, 90};
  phrase.note[2] = {300, 200, 100, 90};
  gk::PhrasePlayer player;
  assert(player.start(phrase, start));
  assert(player.tick(start).count == 1);
  auto batch = player.tick(start + 250);
  assert(batch.count == 2 && !batch.events[0].down && batch.events[1].down);
  // A late previous frame must not shift the next deadline to 450 ms.
  batch = player.tick(start + 400);
  assert(batch.count == 1 && batch.events[0].down && batch.events[0].slot == 2);
  batch = player.tick(start + 500);
  assert(batch.count == 2 && !batch.events[0].down && !batch.events[1].down);
  assert(!player.active());
  assert(player.start(phrase, 1000));
  batch = player.tick(9000);  // a stalled loop drains all starts and releases
  assert(batch.count == 6 && !player.active());
}

void snapshotMigration() {
  gk::SoulSnapshot old{};
  memcpy(old.magic, "DSZA", 4);
  old.version = 1;
  old.bytes = gk::kSoulV1Bytes;
  old.gardenCount = 2;
  old.phraseStartMask = 1;
  old.cents[0] = -498;
  old.cents[1] = 700;
  old.delayMs[1] = 280;
  old.flags = gk::kCustomChord;
  old.chordCount = 1;
  old.chord[0] = 1200;
  old.pulseMs = 650;
  // Actual legacy bytes: no duration/velocity arrays in the input.
  unsigned char legacy[gk::kSoulV1Bytes];
  memcpy(legacy, &old, sizeof(legacy));
  gk::SoulSnapshot upgraded{};
  assert(gk::decodeSoul(legacy, sizeof(legacy), upgraded));
  assert(upgraded.version == 2 && upgraded.bytes == sizeof(upgraded));
  assert(upgraded.cents[0] == -498 && upgraded.delayMs[1] == 280);
  assert(upgraded.holdMs[0] == 420 && upgraded.velocity[1] == 90);
  assert(upgraded.chord[0] == 1200 && upgraded.pulseMs == 650);
  upgraded.holdMs[0] = 80;
  upgraded.holdMs[1] = 1800;
  upgraded.velocity[0] = 55;
  gk::SoulSnapshot loaded{};
  assert(gk::decodeSoul(&upgraded, sizeof(upgraded), loaded));
  assert(memcmp(&upgraded, &loaded, sizeof(loaded)) == 0);
  float cents[] = {(float)loaded.cents[0], (float)loaded.cents[1]};
  gk::Garden garden;
  garden.restore(cents, loaded.delayMs, loaded.phraseStartMask, loaded.gardenCount,
                 loaded.holdMs, loaded.velocity);
  gk::GardenPhrase phrase;
  assert(garden.latestPhrase(&phrase));
  assert(phrase.note[0].holdMs == 80 && phrase.note[0].velocity == 55);
  assert(phrase.note[1].holdMs == 1800);
  garden.noteOn(1, 900, 100);
  assert(garden.startsPhrase(2));
  assert(!gk::decodeSoul(&upgraded, sizeof(upgraded) - 1, loaded));
  upgraded.holdMs[0] = 0;
  assert(!gk::decodeSoul(&upgraded, sizeof(upgraded), loaded));
  upgraded.holdMs[0] = 80;
  upgraded.velocity[1] = 255;
  assert(!gk::decodeSoul(&upgraded, sizeof(upgraded), loaded));
  upgraded.velocity[1] = 90;
  upgraded.phraseStartMask = 0;
  assert(!gk::decodeSoul(&upgraded, sizeof(upgraded), loaded));
  assert(loaded.holdMs[0] == 80);  // a rejected snapshot never changes output
}

}  // namespace

int main() {
  captureAndRecall();
  clocksAndBounds();
  snapshotMigration();
  puts("memory_performance_test: ok");
}
