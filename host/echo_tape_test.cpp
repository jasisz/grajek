#include <assert.h>
#include <math.h>
#include <stdio.h>

#include <algorithm>
#include <vector>

#include "ga_echo.h"

namespace {

constexpr int kSampleRate = 1000;
constexpr int kTapeFrames = 3 * kSampleRate;

struct Fixture {
  std::vector<int16_t> tape = std::vector<int16_t>(kTapeFrames);
  ga::EchoTape echo;

  Fixture() {
    echo.init(tape.data(), (uint32_t)tape.size(), (float)kSampleRate);
    echo.setFeedback(0.0f);
    echo.setLevel(1.0f);
    echo.setWowDepth(0.0f);
  }

  std::vector<float> render(int frames, bool impulseAtStart) {
    std::vector<float> out(frames);
    for (int i = 0; i < frames; ++i) {
      const float in = impulseAtStart && i == 0 ? 1.0f : 0.0f;
      echo.process(&in, &out[i], 1);
    }
    return out;
  }
};

float peak(const std::vector<float>& signal, int first, int last) {
  float result = 0.0f;
  first = std::max(first, 0);
  last = std::min(last, (int)signal.size() - 1);
  for (int i = first; i <= last; ++i)
    result = std::max(result, fabsf(signal[i]));
  return result;
}

void testDottedEighthTap() {
  Fixture f;
  f.echo.setRhythmicTap(0.4f, true);  // 0.75 * 400 ms = 300 ms
  const std::vector<float> out = f.render(520, true);
  assert(peak(out, 296, 304) > 0.10f);
  assert(peak(out, 20, 260) < 0.001f);
}

void testFastTapUsesContinuousFloor() {
  Fixture f;
  // 150 ms would become a comb-like colour, so it rests at 280 ms without a
  // discontinuous octave jump when the learned period crosses a boundary.
  f.echo.setRhythmicTap(0.2f, true);
  const std::vector<float> out = f.render(520, true);
  assert(peak(out, 276, 284) > 0.10f);
  assert(peak(out, 146, 154) < 0.001f);
}

void testFastBoundaryDoesNotJumpAnOctave() {
  Fixture below;
  Fixture above;
  below.echo.setRhythmicTap(0.372f, true);  // floored to 280 ms
  above.echo.setRhythmicTap(0.376f, true);  // 282 ms
  const std::vector<float> a = below.render(420, true);
  const std::vector<float> b = above.render(420, true);
  assert(peak(a, 276, 284) > 0.10f);
  assert(peak(b, 278, 286) > 0.10f);
  assert(peak(a, 390, 419) < 0.001f);
  assert(peak(b, 390, 419) < 0.001f);
}

void testMainReturnStaysThreeSeconds() {
  Fixture f;
  f.echo.setRhythmicTap(0.4f, true);
  const std::vector<float> out = f.render(kTapeFrames + 2, true);
  // The rhythmic head is an additional quiet reply. The write-position head
  // still returns the original sample after the full three-second tape loop.
  assert(out[kTapeFrames] > 0.95f);
}

void testUnlockReturnsGradually() {
  Fixture f;
  f.echo.setRhythmicTap(0.4f, true);
  f.render(1000, false);  // finish the 750 ms lock crossfade

  f.echo.clearTape();
  f.echo.setRhythmicTap(0.4f, false);
  const std::vector<float> justUnlocked = f.render(420, true);
  // It does not jump at lock loss: 300 ms later much of the rhythmic reply
  // is deliberately still present during the 1.5 s return crossfade.
  assert(peak(justUnlocked, 296, 304) > 0.18f);

  f.render(1700, false);  // let the return to the golden head finish
  f.echo.clearTape();
  const std::vector<float> freeAgain = f.render(420, true);
  assert(peak(freeAgain, 296, 304) < 0.001f);
}

}  // namespace

int main() {
  testDottedEighthTap();
  testFastTapUsesContinuousFloor();
  testFastBoundaryDoesNotJumpAnOctave();
  testMainReturnStaysThreeSeconds();
  testUnlockReturnsGradually();
  puts("echo_tape_test: ok");
  return 0;
}
