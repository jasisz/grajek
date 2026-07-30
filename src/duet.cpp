#include "duet.h"

#include <math.h>
#include <stdint.h>

#include "ambient.h"
#include "ga_scales.h"
#include "hal/audio_out.h"
#include "settings.h"
#include "viz.h"

using namespace ga;

namespace {

constexpr int32_t kDuetId = 1400;  // poza siatką, tłem, duchami i dzwonkami
constexpr int kWin = 1024;         // okno analizy ~21 ms @ 48 kHz
// próg głosu (RMS po kMicGain) — do strojenia uchem na sprzęcie razem z
// kMicGain w audio_out.cpp; za niski = duet nuci do szumu wentylatora
constexpr float kVoiceRms = 0.015f;

uint32_t s_analyzed = 0;
int s_stable = 0;
int s_silent = 0;
float s_lastCents = 1e9f;
float s_companion = 1e9f;
bool s_on = false;

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
  s_silent = 0;
  s_lastCents = 1e9f;
  s_companion = 1e9f;
  s_on = false;
}

void humOff(ga::Engine& e) {
  if (s_on) e.noteOff(kDuetId);
  s_on = false;
  s_stable = 0;
  s_companion = 1e9f;
  hal::earSetDuet(false);
  viz::noteOff((int)kDuetId);
}

void tick(ga::Engine& e) {
  if (!hal::earIsOpen()) return;
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
    float acAll[601];
    float best = 0.0f;
    for (int lag = 80; lag <= 600; ++lag) {
      float ac = 0.0f;
      for (int i = 0; i + lag < kWin; i += 2) ac += w[i] * w[i + lag];
      acAll[lag] = ac;
      if (ac > best) best = ac;
    }
    // pierwszy lag bliski maksimum = najniższa wiarygodna podstawa
    int bestLag = 0;
    for (int lag = 80; lag <= 600; ++lag)
      if (acAll[lag] >= 0.90f * best) {
        bestLag = lag;
        break;
      }
    const float clarity = e0 > 0.0f ? best / e0 : 0.0f;
    if (clarity > 0.30f && bestLag > 0) {
      voiced = true;
      hz = 48000.0f / (float)bestLag;
    }
  }

  // telemetria strojenia: raz na sekundę realne liczby na serial —
  // z nich wynika, czy podnosić kMicGain (rms ~0) czy progi (nuci do szumu)
  static uint32_t lastLogMs = 0;
  const uint32_t nowMs = millis();
  if (nowMs - lastLogMs > 1000) {
    lastLogMs = nowMs;
    Serial.printf("grajek: ucho rms=%.4f poziom=%.3f glos=%s hz=%.1f duet=%s\n",
                  rms, hal::earLevel(), voiced ? "TAK" : "nie", hz,
                  s_on ? "nuci" : "cicho");
  }

  if (!voiced) {
    // spółgłoski i oddechy to nie koniec piosenki: schodzimy dopiero po
    // ~300 ms prawdziwej ciszy (analiza tyka co ~21 ms)
    if (s_on && ++s_silent >= 14) humOff(e);
    return;
  }
  s_silent = 0;

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
  if (!s_on || fabsf(comp - s_companion) > 25.0f) {
    // zaśpiewana (przyciągnięta do skali) nuta zostaje we wspomnieniach:
    // machanie po śpiewaniu rozsypuje twoją własną melodię
    ambient::gardenPush(snapped);
    // legato: to samo id + glide + miękki PURE przez kanapkę FIFO;
    // im głośniej śpiewasz, tym śmielej nuci towarzysz
    const float vel = 0.30f + fminf(0.40f, rms * 3.0f);
    e.setParam(Param::TimbrePreset, 0.0f);
    e.setParam(Param::GlideSec, 0.08f);
    e.noteOn(kDuetId, comp, vel);
    e.setParam(Param::GlideSec, 0.0f);
    e.setParam(Param::TimbrePreset, (float)settings::preset());
    s_companion = comp;
    if (!s_on) viz::toast("duet!");
    s_on = true;
    hal::earSetDuet(true);
    viz::noteOn((int)kDuetId, comp, 0.5f);
  }
}

}  // namespace duet
