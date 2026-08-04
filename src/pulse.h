// The heart: pulse entrainment, ported from the host (live_main.cpp).
// The box never imposes a tempo — it follows yours. Play a few even
// notes and a quiet MUSICBOX heartbeat joins in, phase-locked to YOUR
// presses (a PLL, not a metronome); stop, or drift into rubato, and it
// lets go within a few beats. The ghosts remember the last pulse and
// sow their memories on that beat grid.
#pragma once
#include "ga_engine.h"

namespace pulse {

void init(ga::Engine* engine);
void onOnset();  // every real key-down (grid notes only)
// App-owned context is passed explicitly, keeping Pulse independent from
// ambient/settings and making the dependency graph acyclic.
void tick(bool suspended, const float* chordCents,
          int chordCount);  // once per main-loop pass
// Wrap-extended Arduino clock shared with phrase scheduling.
double nowSec();

bool ticking();
// the ghost beat grid: last stable period (0 = never entrained) and the
// time of the last onset, both on the monotonic extended clock
double memoryPeriodSec();
double lastOnsetSec();
// soul: yesterday's period comes back with the box (memory only — the
// heart itself still waits for real even presses)
void restoreMemory(double periodSec);

}  // namespace pulse
