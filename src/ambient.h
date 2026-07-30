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
// wiatr: machanie wyrywa z ogrodu jedno wspomnienie (świeże częściej,
// czasem oktawę/kwintę wyżej — jak duchy). false = ogród jeszcze pusty.
bool gardenPluck(float* cents);

// background chord (default 1/1 + 3/2; hand-picking makes the choice law)
void backgroundToggleNote(float cents);
void backgroundSetEnabled(bool on);
bool backgroundEnabled();
int backgroundCount();
// wznowienie tła po engine.allNotesOff() (wyjście z trybu przez menu
// ubijało tampurę na zawsze — pogoda dogrywa tylko kwintę przy zmianie)
void backgroundRefresh();

// bases the weather drifts around
void setCutoffBase(float hz);
// przechył do/od siebie: 0 = sucho i blisko, 0.5 = neutralnie, 1 = daleko
// w pogłosie i echu; pogoda mnoży przez to swoje wartości wet/level
void setSpaceBase(float space01);

// --- okno dla wizualizacji (tylko odczyt, bez side-effectów w audio) ---
// oddech pogody 0..1 (1 = cisza, pudełko oddycha najgłębiej)
float breathe01();
// aktualne nuty akordu tła (pagórki na horyzoncie)
int backgroundNoteCount();
float backgroundNoteCents(int i);
// true dokładnie raz, gdy ogród właśnie zagrał wspomnienie; *cents = które
bool pollGhost(float* cents);
// the player's current preset index: ghosts play with it, while the
// background keeps its own DRONE timbre (via a FIFO queue sandwich —
// a percussive player preset must not silently kill the drone)
void setPreset(int idx);

}  // namespace ambient
