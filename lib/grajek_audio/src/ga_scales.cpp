#include "ga_scales.h"
#include <math.h>

namespace ga {

namespace {

struct Ratio { int num, den; };

// 14 ascending 11-limit intervals within the octave, in the spirit of
// Partch's tonality diamond.
constexpr Ratio kJi11[kGridCols] = {
    {1, 1}, {12, 11}, {9, 8}, {7, 6}, {6, 5}, {5, 4}, {4, 3},
    {11, 8}, {3, 2}, {14, 9}, {8, 5}, {5, 3}, {7, 4}, {11, 6},
};

struct Ji11Cents {
  float c[kGridCols];
  Ji11Cents() {
    for (int i = 0; i < kGridCols; ++i)
      c[i] = 1200.0f * log2f((float)kJi11[i].num / (float)kJi11[i].den);
  }
};

const float* ji11Cents() {
  static Ji11Cents t;
  return t.c;
}

int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// The "happy" subsets, in pure intonation:
// major pentatonic 1/1 9/8 5/4 3/2 5/3 — the kalimba/wind-chime scale
const float kPenta[5] = {0.0f, 203.9f, 386.3f, 702.0f, 884.4f};
// Ptolemy's intense diatonic (just major): 1/1 9/8 5/4 4/3 3/2 5/3 15/8
const float kMajor[7] = {0.0f, 203.9f, 386.3f, 498.0f,
                         702.0f, 884.4f, 1088.3f};

float stepTable(const float* t, int n, int degree) {
  const int oct = degree / n;
  return t[degree % n] + 1200.0f * (float)oct;
}

// Scale implementations. The EDO grids climb by fourths, so their 4x14 keys
// span only ~1.7-2.3 octaves against ~3.3-3.9 for the octave-row scales. Their
// one-octave lift keeps the keyboard centre near the happy JI scales.
float edo12Step(int step) { return 1200.0f + (float)step * 100.0f; }
float edo19Step(int step) {
  return 1200.0f + (float)step * (1200.0f / 19.0f);
}
float edo31Step(int step) {
  return 1200.0f + (float)step * (1200.0f / 31.0f);
}
float ji11Step(int step) {
  return ji11Cents()[step % kGridCols] +
         1200.0f * (float)(step / kGridCols);
}
float pentaStep(int step) { return stepTable(kPenta, 5, step); }
float majorStep(int step) { return stepTable(kMajor, 7, step); }
float wolfStep(int step) { return 1200.0f + (float)step * 62.0f; }

float edo12Grid(int col, int row) {
  return 1200.0f + (float)(col + row * 5) * 100.0f;
}
float edo19Grid(int col, int row) {
  return 1200.0f + (float)(col + row * 8) * (1200.0f / 19.0f);
}
float edo31Grid(int col, int row) {
  return 1200.0f + (float)(col + row * 13) * (1200.0f / 31.0f);
}
float ji11Grid(int col, int row) {
  return ji11Cents()[col] + (float)row * 1200.0f;
}
float pentaGrid(int col, int row) {
  return stepTable(kPenta, 5, col + row * 2);
}
float majorGrid(int col, int row) {
  return stepTable(kMajor, 7, col + row * 2);
}
float wolfGrid(int col, int row) {
  // Quarter-tone-ish clusters, rows a sour near-tritone apart. The only
  // "fifth" (column difference 11) is the historical 682-cent wolf.
  return 1200.0f + (float)col * 62.0f + (float)row * 637.0f;
}

// Preserve the old default arms exactly. They intentionally differ from the
// EDO12 plugin: an invalid ID has no register lift, and its grid ignores rows.
float invalidStep(int step) { return (float)step * 100.0f; }
float invalidGrid(int col, int) { return (float)col * 100.0f; }

// Discovery order is part of the firmware's legacy NVS format ("skala" stores
// this array index), whereas ScaleId itself is persisted by the host.
constexpr ScalePlugin kScalePlugins[] = {
    {ScaleId::PENTA,
     {"PENTA JI", "major pentatonic, row = +2 steps (thirds)"},
     5, pentaStep, pentaGrid},
    {ScaleId::MAJOR,
     {"MAJOR JI", "just major, row = +2 steps (thirds)"},
     7, majorStep, majorGrid},
    {ScaleId::EDO12,
     {"12-EDO", "row = +5 steps (fourth)"},
     12, edo12Step, edo12Grid},
    {ScaleId::EDO19,
     {"19-EDO", "row = +8 steps (fourth)"},
     19, edo19Step, edo19Grid},
    {ScaleId::EDO31,
     {"31-EDO", "row = +13 steps (fourth)"},
     31, edo31Step, edo31Grid},
    {ScaleId::JI11,
     {"JI 11-limit", "14 ratios/octave, row = +octave"},
     kGridCols, ji11Step, ji11Grid},
    {ScaleId::WOLF,
     {"WOLF", "62 c steps, row = +637 c - nothing is pure"},
     19, wolfStep, wolfGrid},
};

constexpr size_t kNumScalePlugins =
    sizeof(kScalePlugins) / sizeof(kScalePlugins[0]);

// Public compatibility fallback for an invalid ScaleId.
constexpr ScalePlugin kInvalidScale = {
    ScaleId::EDO12,
    {"12-EDO", "row = +5 steps (fourth)"},
    12,
    invalidStep,
    invalidGrid,
};

static_assert((uint8_t)ScaleId::EDO12 == 0 &&
                  (uint8_t)ScaleId::EDO19 == 1 &&
                  (uint8_t)ScaleId::EDO31 == 2 &&
                  (uint8_t)ScaleId::JI11 == 3 &&
                  (uint8_t)ScaleId::PENTA == 4 &&
                  (uint8_t)ScaleId::MAJOR == 5 &&
                  (uint8_t)ScaleId::WOLF == 6 &&
                  (uint8_t)ScaleId::Count == 7,
              "ScaleId values are a persistence format");
static_assert(kNumScalePlugins == (size_t)ScaleId::Count,
              "every persisted ScaleId needs exactly one plugin");
static_assert(kScalePlugins[0].id == ScaleId::PENTA &&
                  kScalePlugins[1].id == ScaleId::MAJOR &&
                  kScalePlugins[2].id == ScaleId::EDO12 &&
                  kScalePlugins[3].id == ScaleId::EDO19 &&
                  kScalePlugins[4].id == ScaleId::EDO31 &&
                  kScalePlugins[5].id == ScaleId::JI11 &&
                  kScalePlugins[6].id == ScaleId::WOLF,
              "scale plugin order is the legacy firmware NVS format");

}  // namespace

size_t scalePluginCount() { return kNumScalePlugins; }

const ScalePlugin& scalePluginAt(size_t index) {
  return index < kNumScalePlugins ? kScalePlugins[index] : kScalePlugins[0];
}

const ScalePlugin& scalePlugin(ScaleId id) {
  for (size_t i = 0; i < kNumScalePlugins; ++i)
    if (kScalePlugins[i].id == id) return kScalePlugins[i];
  return kInvalidScale;
}

size_t scalePluginIndex(ScaleId id) {
  for (size_t i = 0; i < kNumScalePlugins; ++i)
    if (kScalePlugins[i].id == id) return i;
  return 0;
}

const ScaleInfo& scaleInfo(ScaleId s) { return scalePlugin(s).info; }

int scaleStepsPerOctave(ScaleId s) { return scalePlugin(s).stepsPerOctave; }

float scaleStepCents(ScaleId s, int step) {
  if (step < 0) step = 0;
  return scalePlugin(s).stepFn(step);
}

float gridToCents(ScaleId s, int col, int row) {
  col = clampi(col, 0, kGridCols - 1);
  row = clampi(row, 0, kGridRows - 1);
  return scalePlugin(s).gridFn(col, row);
}

}  // namespace ga
