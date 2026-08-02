#include <assert.h>
#include <stdint.h>

#include "../src/modes/face_down_gesture.h"

int main() {
  using Event = FaceDownGesture::Event;

  FaceDownGesture g;
  assert(g.update(100, -1.0f, false) == Event::None);
  assert(g.update(1299, -1.0f, false) == Event::None);
  assert(g.update(1300, -1.0f, false) == Event::Sleep);
  assert(g.update(1400, -1.0f, true) == Event::None);

  // Hysteresis keeps a small bump from waking it; a real lift does.
  assert(g.update(1500, -0.70f, true) == Event::None);
  assert(g.update(1600, -0.50f, true) == Event::Wake);

  // A brief flip and a key wake-up both require a fresh full dwell.
  assert(g.update(2000, -1.0f, false) == Event::None);
  assert(g.update(2500, 0.9f, false) == Event::None);
  assert(g.update(2600, -1.0f, false) == Event::None);
  assert(g.update(3799, -1.0f, false) == Event::None);
  assert(g.update(3800, -1.0f, false) == Event::Sleep);
  g.restartDwell();
  assert(g.update(3900, -1.0f, false) == Event::None);
  assert(g.update(5099, -1.0f, false) == Event::None);
  assert(g.update(5100, -1.0f, false) == Event::Sleep);

  // Unsigned elapsed time remains correct across millis() wraparound.
  FaceDownGesture wrap;
  constexpr uint32_t start = UINT32_MAX - 500;
  assert(wrap.update(start, -1.0f, false) == Event::None);
  assert(wrap.update(start + 1199u, -1.0f, false) == Event::None);
  assert(wrap.update(start + 1200u, -1.0f, false) == Event::Sleep);
}
