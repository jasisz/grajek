#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "../src/modes/face_down_gesture.h"
#include "../src/modes/idle_sleep.h"

namespace {

constexpr uint32_t kIdle = IdleSleep::kIdleMs;

void testSilenceEventuallySleeps() {
  IdleSleep idle;
  assert(idle.update(1000, false) == false);   // seeds the clock
  assert(idle.update(1000 + kIdle - 1, false) == false);
  assert(idle.update(1000 + kIdle, false) == true);
  // It fires exactly once; the box is not asked to fall asleep twice.
  assert(idle.update(1000 + kIdle + 1, false) == false);
  assert(idle.update(1000 + 5 * kIdle, false) == false);
}

void testActivityRearmsTheClock() {
  IdleSleep idle;
  idle.update(0, false);
  idle.noteActivity(kIdle - 10);           // a key just before bedtime
  assert(idle.update(kIdle, false) == false);
  assert(idle.update(2 * kIdle - 11, false) == false);
  assert(idle.update(2 * kIdle - 10, false) == true);
}

void testSleepingBoxIsLeftAlone() {
  IdleSleep idle;
  idle.update(0, false);
  // Asleep for any reason (a face-down dwell, say): the timer must not stack
  // another lullaby on top, and must stay quiet after that lullaby ends.
  assert(idle.update(kIdle, true) == false);
  assert(idle.update(3 * kIdle, false) == false);
  // Only a person rearms it.
  idle.noteActivity(3 * kIdle);
  assert(idle.update(4 * kIdle - 1, false) == false);
  assert(idle.update(4 * kIdle, false) == true);
}

void testUntouchedBoxStillFallsAsleep() {
  IdleSleep idle;  // switched on and never played
  assert(idle.update(500, false) == false);
  assert(idle.update(500 + kIdle, false) == true);
}

void testWrapAround() {
  IdleSleep idle;
  const uint32_t nearWrap = 0xFFFFFFFFu - (kIdle / 2);
  idle.noteActivity(nearWrap);
  assert(idle.update(nearWrap + kIdle - 1, false) == false);  // wraps past 0
  assert(idle.update(nearWrap + kIdle, false) == true);
}

// An idle sleep begins with the box sitting face UP. The face-down gesture
// must not read that as a lift and cancel the lullaby on the very next
// sample — the bug this rule exists to prevent.
void testIdleSleepFaceUpIsNotAWake() {
  FaceDownGesture g;
  using Event = FaceDownGesture::Event;
  assert(g.update(100, 0.98f, false) == Event::None);   // sitting face up
  assert(g.update(200, 0.98f, true) == Event::None);    // now asleep, face up
  assert(g.update(300, 0.98f, true) == Event::None);
  // Turning it over and back up again IS a deliberate lift, so that wakes.
  assert(g.update(400, -0.95f, true) == Event::None);
  assert(g.update(500, 0.95f, true) == Event::Wake);
}

}  // namespace

int main() {
  testSilenceEventuallySleeps();
  testActivityRearmsTheClock();
  testSleepingBoxIsLeftAlone();
  testUntouchedBoxStillFallsAsleep();
  testWrapAround();
  testIdleSleepFaceUpIsNotAWake();
  puts("idle_sleep_test: ok");
  return 0;
}
