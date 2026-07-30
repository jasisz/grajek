#include "duet.h"

#include <Arduino.h>
#include <math.h>

#include "ambient.h"
#include "ga_scales.h"
#include "hal/audio_out.h"
#include "settings.h"
#include "viz.h"

using namespace ga;

namespace {

constexpr int kWin = 1024;  // analysis window: 64 ms @ 16 kHz
// voice threshold (RMS) — tune by ear on hardware together with the mic
// magnification; too low and the box duets with the fan
constexpr float kVoiceRms = 0.015f;
// autocorrelation lags for 80..800 Hz at the 16 kHz mic rate — the top
// must clear a child's playful head voice (C5+), or everything above
// 500 Hz gets detected an octave down and the companion lands an octave
// and a third UNDER the singer
constexpr int kLagLo = 20;
constexpr int kLagHi = 200;

uint32_t s_analyzed = 0;
int s_stable = 0;
float s_lastCents = 1e9f;
float s_lastComp = 1e9f;

struct Ev { uint32_t off; float cents; };
constexpr int kTrackCap = 48;
Ev s_track[kTrackCap];
int s_trackN = 0;

float snapToGrid(float cents) {
  // najbliższa komórka pełnej siatki 14x4 aktualnej skali
  float best = 0.0f, bestD = 1e9f;
  for (int row = 0; row < kGridRows; ++row)
    for (int col = 0; col < kGridCols; ++col) {
      const float c = gridToCents(settings::scale(), col, row);
      const float d = fabsf(c - cents);
      if (d < bestD) {
        bestD = d;
        best = c;
      }
    }
  return best;
}

}  // namespace

namespace duet {

void reset() {
  s_analyzed = 0;
  s_stable = 0;
  s_lastCents = 1e9f;
  s_lastComp = 1e9f;
  s_trackN = 0;
}

int trackCount() { return s_trackN; }
uint32_t trackOffset(int i) {
  return (i >= 0 && i < s_trackN) ? s_track[i].off : 0;
}
float trackCents(int i) {
  return (i >= 0 && i < s_trackN) ? s_track[i].cents : 0.0f;
}

void tick() {
  if (!hal::earWindowActive()) return;
  const uint32_t n = hal::earCaptured();
  if (n < (uint32_t)kWin || n < s_analyzed + (uint32_t)kWin) return;
  s_analyzed = n;

  static float w[kWin];  // core 1, jeden wołający — static zdejmuje 4 KB ze stosu
  hal::earWindow(w, kWin);

  // energia + autokorelacja we float — double na S3 liczy się programowo
  float e0 = 0.0f;
  for (int i = 0; i < kWin; ++i) e0 += w[i] * w[i];
  const float rms = sqrtf(e0 / (float)kWin);
  bool voiced = false;
  float hz = 0.0f;

  if (rms > kVoiceRms) {
    float acAll[kLagHi + 2];
    float best = 0.0f;
    for (int lag = kLagLo; lag <= kLagHi + 1; ++lag) {
      float ac = 0.0f;
      for (int i = 0; i + lag < kWin; i += 2) ac += w[i] * w[i + lag];
      acAll[lag] = ac;
      if (lag <= kLagHi && ac > best) best = ac;
    }
    // pierwszy lag bliski maksimum = najniższa wiarygodna podstawa
    int bestLag = 0;
    for (int lag = kLagLo; lag <= kLagHi; ++lag)
      if (acAll[lag] >= 0.90f * best) {
        bestLag = lag;
        break;
      }
    // clarity normalized to the numerator's own term count: the sum uses
    // every 2nd sample over (kWin-lag) — dividing by full-window energy
    // made the threshold mean 0.62 at high lags but 0.75 at low ones, so
    // a low parent voice needed to be much cleaner than a high child's
    float e0even = 0.0f;
    for (int i = 0; i < kWin; i += 2) e0even += w[i] * w[i];
    const float norm =
        bestLag > 0 ? e0even * (float)(kWin - bestLag) / (float)kWin : 0.0f;
    const float clarity = norm > 0.0f ? best / norm : 0.0f;
    if (clarity > 0.30f && bestLag > 0) {
      // parabolic peak interpolation: an integer lag quantizes pitch by
      // up to ~27-53 c up the band — enough to snap to the WRONG 31-EDO
      // step; the parabola through the neighbours recovers the fraction
      float lagF = (float)bestLag;
      if (bestLag > kLagLo) {
        const float ym = acAll[bestLag - 1], y0 = acAll[bestLag],
                    yp = acAll[bestLag + 1];
        const float den = ym - 2.0f * y0 + yp;
        if (fabsf(den) > 1e-9f) {
          const float d = 0.5f * (ym - yp) / den;
          if (d > -1.0f && d < 1.0f) lagF += d;
        }
      }
      voiced = true;
      hz = hal::earSampleRate() / lagF;
    }
  }

  // telemetria strojenia: raz na sekundę realne liczby na serial —
  // z nich wynika, czy podnosić magnification (rms ~0) czy progi
  static uint32_t lastLogMs = 0;
  const uint32_t nowMs = millis();
  if (nowMs - lastLogMs > 1000) {
    lastLogMs = nowMs;
    Serial.printf("grajek: ucho rms=%.4f poziom=%.3f glos=%s hz=%.1f nut=%d\n",
                  rms, hal::earLevel(), voiced ? "TAK" : "nie", hz, s_trackN);
  }

  if (!voiced) return;  // ciszę i spółgłoski ocenia polityka okien w ŚPIEWIE

  float cents = 1200.0f * log2f(hz / settings::baseHz());
  // strażnik oktawy: jak detektor przeskoczył o całą oktawę między tikami,
  // zwiń odczyt z powrotem do poprzedniego rejestru
  if (s_lastCents < 1e8f) {
    const float d = cents - s_lastCents;
    if (fabsf(fabsf(d) - 1200.0f) < 90.0f) cents -= copysignf(1200.0f, d);
  }
  if (fabsf(cents - s_lastCents) < 80.0f) ++s_stable;
  else s_stable = 1;
  s_lastCents = cents;
  if (s_stable < 2) return;  // toleruje vibrato, ignoruje pojedyncze piski

  // głos widoczny na żywo: kometa leci po łące na wysokości śpiewu
  viz::voice(cents, rms * 6.0f);

  const float snapped = snapToGrid(cents);
  float comp = snapToGrid(snapped - 350.0f);  // celuj tercję niżej
  if (fabsf(comp - snapped) < 30.0f)
    comp = snapToGrid(snapped - 550.0f);  // awaryjnie coś koło kwarty
  if (comp > snapped - 100.0f)
    comp = snapped - 1200.0f;  // grid floor reached: the octave below,
                               // never unison — the companion sits UNDER you
  if (s_trackN == 0 || fabsf(comp - s_lastComp) > 25.0f) {
    // zaśpiewana (przyciągnięta do skali) nuta zostaje we wspomnieniach:
    // machanie po śpiewaniu rozsypuje twoją własną melodię
    ambient::gardenPush(snapped);
    if (s_trackN < kTrackCap) s_track[s_trackN++] = {n, comp};
    s_lastComp = comp;
  }
}

}  // namespace duet
