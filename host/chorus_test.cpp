#include <assert.h>
#include <math.h>
#include <string.h>

#include "ga_chorus.h"

namespace {

constexpr int kFrames = 2048;

void depthAndBypassTest() {
  ga::Chorus chorus;
  chorus.init(48000.0f);
  static_assert(ga::Chorus::kBufferFrames <= 1024);
  static_assert(ga::Chorus::kBufferBytes <= 4096);
  assert(chorus.depth() == 0.0f);

  float signal[64];
  float original[64];
  for (int i = 0; i < 64; ++i) signal[i] = (float)(i - 31) / 64.0f;
  memcpy(original, signal, sizeof(signal));
  chorus.process(signal, 64);
  assert(memcmp(signal, original, sizeof(signal)) == 0);

  chorus.setDepth(-1.0f);
  assert(chorus.depth() == 0.0f);
  chorus.setDepth(2.0f);
  assert(chorus.depth() == 1.0f);
  chorus.setDepth(NAN);
  assert(chorus.depth() == 0.0f);
}

void dualTapImpulseTest() {
  ga::Chorus chorus;
  chorus.init(48000.0f);
  chorus.setDepth(1.0f);
  float signal[1200] = {0};
  signal[0] = 1.0f;
  chorus.process(signal, 1200);

  // Dry impulse is untouched. Two separated fractional-delay heads create
  // two wet arrivals rather than one fixed comb tap.
  assert(signal[0] == 1.0f);
  int first = -1;
  int last = -1;
  float wetEnergy = 0.0f;
  for (int i = 1; i < 1200; ++i) {
    wetEnergy += signal[i] * signal[i];
    if (fabsf(signal[i]) > 1e-5f) {
      if (first < 0) first = i;
      last = i;
    }
  }
  assert(wetEnergy > 0.01f);
  assert(first > 300);
  assert(last - first > 150);
}

void blockPartitionTest() {
  ga::Chorus whole;
  ga::Chorus chunked;
  whole.init(48000.0f);
  chunked.init(48000.0f);
  whole.setDepth(0.73f);
  chunked.setDepth(0.73f);

  float a[kFrames];
  float b[kFrames];
  for (int i = 0; i < kFrames; ++i) {
    const float sample = 0.35f * sinf(6.2831853f * 233.0f * i / 48000.0f) +
                         0.12f * sinf(6.2831853f * 401.0f * i / 48000.0f);
    a[i] = b[i] = sample;
  }
  whole.process(a, kFrames);
  for (int pos = 0; pos < kFrames;) {
    int n = (pos % 113) + 1;
    if (n > kFrames - pos) n = kFrames - pos;
    chunked.process(b + pos, n);
    pos += n;
  }
  for (int i = 0; i < kFrames; ++i) {
    assert(isfinite(a[i]));
    assert(fabsf(a[i] - b[i]) < 1e-7f);
  }

  // Active -> bypass fades rather than clicking. Once settled, bypass is
  // again bit-exact, and the ring has followed silence instead of retaining
  // an old fragment that could return on re-enable.
  whole.setDepth(0.0f);
  float settle[8192] = {};
  whole.process(settle, 8192);
  float tail[37];
  float untouched[37];
  for (int i = 0; i < 37; ++i) tail[i] = untouched[i] = 0.01f * i;
  whole.process(tail, 37);
  assert(memcmp(tail, untouched, sizeof(tail)) == 0);
}

}  // namespace

int main() {
  depthAndBypassTest();
  dualTapImpulseTest();
  blockPartitionTest();
}
