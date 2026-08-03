#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "ga_scales.h"

using namespace ga;

namespace {

const float kPenta[5] = {0.0f, 203.9f, 386.3f, 702.0f, 884.4f};
const float kMajor[7] = {0.0f, 203.9f, 386.3f, 498.0f,
                         702.0f, 884.4f, 1088.3f};
const int kJi11[14][2] = {
    {1, 1}, {12, 11}, {9, 8}, {7, 6}, {6, 5}, {5, 4}, {4, 3},
    {11, 8}, {3, 2}, {14, 9}, {8, 5}, {5, 3}, {7, 4}, {11, 6},
};

int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

float tableStep(const float* table, int count, int step) {
  return table[step % count] + 1200.0f * (float)(step / count);
}

float ji11Cents(int degree) {
  return 1200.0f *
         log2f((float)kJi11[degree][0] / (float)kJi11[degree][1]);
}

float legacyLift(ScaleId id) {
  switch (id) {
    case ScaleId::EDO12:
    case ScaleId::EDO19:
    case ScaleId::EDO31:
    case ScaleId::WOLF: return 1200.0f;
    default:            return 0.0f;
  }
}

int legacyStepsPerOctave(ScaleId id) {
  switch (id) {
    case ScaleId::EDO12: return 12;
    case ScaleId::EDO19: return 19;
    case ScaleId::EDO31: return 31;
    case ScaleId::JI11:  return 14;
    case ScaleId::PENTA: return 5;
    case ScaleId::MAJOR: return 7;
    case ScaleId::WOLF:  return 19;
    default:             return 12;
  }
}

float legacyStepCents(ScaleId id, int step) {
  if (step < 0) step = 0;
  const float lift = legacyLift(id);
  switch (id) {
    case ScaleId::EDO12: return lift + (float)step * 100.0f;
    case ScaleId::EDO19:
      return lift + (float)step * (1200.0f / 19.0f);
    case ScaleId::EDO31:
      return lift + (float)step * (1200.0f / 31.0f);
    case ScaleId::JI11:
      return ji11Cents(step % 14) + 1200.0f * (float)(step / 14);
    case ScaleId::PENTA: return tableStep(kPenta, 5, step);
    case ScaleId::MAJOR: return tableStep(kMajor, 7, step);
    case ScaleId::WOLF:  return lift + (float)step * 62.0f;
    default:             return (float)step * 100.0f;
  }
}

float legacyGridToCents(ScaleId id, int col, int row) {
  col = clampi(col, 0, 13);
  row = clampi(row, 0, 3);
  const float lift = legacyLift(id);
  switch (id) {
    case ScaleId::EDO12:
      return lift + (float)(col + row * 5) * 100.0f;
    case ScaleId::EDO19:
      return lift + (float)(col + row * 8) * (1200.0f / 19.0f);
    case ScaleId::EDO31:
      return lift + (float)(col + row * 13) * (1200.0f / 31.0f);
    case ScaleId::JI11:  return ji11Cents(col) + (float)row * 1200.0f;
    case ScaleId::PENTA: return tableStep(kPenta, 5, col + row * 2);
    case ScaleId::MAJOR: return tableStep(kMajor, 7, col + row * 2);
    case ScaleId::WOLF:
      return lift + (float)col * 62.0f + (float)row * 637.0f;
    default: return (float)col * 100.0f;
  }
}

void assertNear(float actual, float expected) {
  assert(fabsf(actual - expected) <= 0.001f);
}

}  // namespace

int main() {
  static_assert((uint8_t)ScaleId::EDO12 == 0, "persisted ID changed");
  static_assert((uint8_t)ScaleId::EDO19 == 1, "persisted ID changed");
  static_assert((uint8_t)ScaleId::EDO31 == 2, "persisted ID changed");
  static_assert((uint8_t)ScaleId::JI11 == 3, "persisted ID changed");
  static_assert((uint8_t)ScaleId::PENTA == 4, "persisted ID changed");
  static_assert((uint8_t)ScaleId::MAJOR == 5, "persisted ID changed");
  static_assert((uint8_t)ScaleId::WOLF == 6, "persisted ID changed");
  static_assert((uint8_t)ScaleId::Count == 7, "persisted ID changed");

  const ScaleId discoveryOrder[] = {
      ScaleId::PENTA, ScaleId::MAJOR, ScaleId::EDO12, ScaleId::EDO19,
      ScaleId::EDO31, ScaleId::JI11, ScaleId::WOLF,
  };
  assert(scalePluginCount() == 7);
  bool seen[7] = {};
  for (size_t i = 0; i < scalePluginCount(); ++i) {
    const ScalePlugin& plugin = scalePluginAt(i);
    assert(plugin.id == discoveryOrder[i]);
    assert(scalePluginIndex(plugin.id) == i);
    assert(&scalePlugin(plugin.id) == &plugin);
    assert(plugin.info.name != nullptr && plugin.info.desc != nullptr);
    assert(plugin.stepFn != nullptr && plugin.gridFn != nullptr);
    const uint8_t raw = (uint8_t)plugin.id;
    assert(raw < 7 && !seen[raw]);
    seen[raw] = true;
  }
  for (bool present : seen) assert(present);

  const char* names[] = {"12-EDO", "19-EDO", "31-EDO", "JI 11-limit",
                         "PENTA JI", "MAJOR JI", "WOLF"};
  const int steps[] = {12, 19, 31, 14, 5, 7, 19};
  const int sampleSteps[] = {-3, 0, 1, 4, 5, 6, 7, 12, 14, 19, 31, 62};
  const int sampleCols[] = {-2, 0, 1, 13, 15};
  const int sampleRows[] = {-2, 0, 1, 3, 5};
  for (int raw = 0; raw < (int)ScaleId::Count; ++raw) {
    const ScaleId id = (ScaleId)raw;
    assert(strcmp(scaleInfo(id).name, names[raw]) == 0);
    assert(scaleStepsPerOctave(id) == steps[raw]);
    assert(scaleStepsPerOctave(id) == legacyStepsPerOctave(id));
    for (int step : sampleSteps)
      assertNear(scaleStepCents(id, step), legacyStepCents(id, step));
    for (int col : sampleCols)
      for (int row : sampleRows)
        assertNear(gridToCents(id, col, row),
                   legacyGridToCents(id, col, row));
  }

  const ScaleId invalid = (ScaleId)255;
  assert(strcmp(scaleInfo(invalid).name, "12-EDO") == 0);
  assert(scaleStepsPerOctave(invalid) == 12);
  assertNear(scaleStepCents(invalid, -3), 0.0f);
  assertNear(scaleStepCents(invalid, 2), 200.0f);
  assertNear(gridToCents(invalid, -2, -2), 0.0f);
  assertNear(gridToCents(invalid, 15, 5), 1300.0f);
  assert(scalePluginIndex(invalid) == 0);
  assert(scalePluginAt(scalePluginCount()).id == ScaleId::PENTA);
  return 0;
}
