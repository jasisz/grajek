// The ambient brain: everything the box does around the player's input —
// the background chord (tampura), the weather (breathing + slow tides) and
// the ghost garden (re-sowing the player's own notes after silence).
// Runs from the main loop (core 1); talks to the engine only through its
// lock-free API and to the effect chain through atomic setters.
#pragma once
#include "ga_engine.h"

namespace ambient {

void init(ga::Engine* engine);
void tick();  // call once per main-loop pass

// presence: any human key press pauses ghost sowing and bows out ghosts
void notePresence();
// a really played grid note, worth remembering for the ghosts
void gardenPush(float cents);

// background chord (default 1/1 + 3/2; hand-picking makes the choice law)
void backgroundToggleNote(float cents);
void backgroundSetEnabled(bool on);
bool backgroundEnabled();
int backgroundCount();

// bases the weather drifts around
void setCutoffBase(float hz);
// the engine's current attack — ghosts restore it after their soft attack
void setPresetAttack(float sec);

}  // namespace ambient
