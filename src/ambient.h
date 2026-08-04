// The ambient brain: everything the box does around the player's input —
// the background chord (tampura), the weather (breathing + slow tides) and
// the ghost garden (re-sowing the player's own notes after silence).
// Runs from the main loop (core 1); talks to the engine only through its
// lock-free API and to the effect chain through atomic setters.
#pragma once
#include <stdint.h>

#include "ga_engine.h"
#include "gk_background.h"
#include "gk_garden.h"

namespace ambient {

constexpr int kGardenCapacity = gk::kGardenCapacity;
constexpr int kGardenPhraseMax = gk::kGardenPhraseMax;
using GardenPhraseNote = gk::GardenPhraseNote;
using GardenPhrase = gk::GardenPhrase;

// The app coordinator supplies the remembered beat grid. Ambient no longer
// reaches back into the pulse module, which keeps the dependency graph one-way.
struct BeatGrid {
  double nowSec = 0.0;
  double periodSec = 0.0;
  double lastOnsetSec = 0.0;
  bool ticking = false;
};

enum class SaveRequest : uint8_t { None, Soul, SoulAndSettings };

void init(ga::Engine* engine);
void tick(const BeatGrid& beatGrid);  // call once per main-loop pass

// Persistence is an application-level side effect. Ambient only schedules a
// natural quiet point; main performs the actual NVS writes and reports back.
SaveRequest saveRequest();
void saveFinished(SaveRequest request, bool success);

// presence: any human key press pauses ghost sowing and bows out ghosts
void notePresence();
// Physical keyboard state, fed globally even when a mode switch suppresses
// the musical event. Deferred NVS writes wait until every key is released.
void keyState(int id, bool down);
// A really played note. Notes close in time are captured as one phrase;
// silence, six notes or ~5 s closes it. This includes sung notes.
void gardenPush(float cents);
// Wind: one swing plucks a whole short phrase (fresh phrases more often).
// One coherent transposition is chosen for the phrase; dir steers it up/down.
// false = garden still empty.
bool gardenPluck(GardenPhrase* phrase, float dir);
// read-only view of the memory ring for the visualization (seed replanting
// after a scene reset); index 0 = oldest remembered note
int gardenCount();
int gardenPhraseCount();
float gardenCents(int idxOldest);
uint16_t gardenDelayMs(int idxOldest);
bool gardenStartsPhrase(int idxOldest);
// Soul: restore the flattened phrase ring from NVS. Restored memories do NOT
// count as playing — ghosts continue sessions, they never start them.
void gardenRestore(const float* cents, const uint16_t* delayMs,
                   uint32_t phraseStartMask, int n);
// soul: one remembered note a few seconds after waking, unless the child
// starts playing first. Power-on is the human act that earns the greeting.
void scheduleGreeting();

// GOODNIGHT: laid face-down after playing, the garden sings itself to
// sleep — the day's phrases replayed once, slower and darker as they go,
// then real silence (tampura included). Lifting the box or any key wakes
// it instantly. It NEVER wakes by itself, and never starts a lullaby
// without play this session — the ritual answers a deliberate gesture.
bool lullabyStart();  // false when this session has nothing to remember
void lullabyAbort();   // lifted / played: wake now
bool lullabyActive();  // Singing or the sleeping silence after

// Background presets are a fixed descriptor registry: silence, root, the
// breathing fifth drone (default), and a three-note harmonic-seventh halo.
// Custom-chord hooks remain for alternate controllers and old soul snapshots.
void backgroundToggleNote(float cents);
void backgroundSetPreset(gk::BackgroundId preset);
gk::BackgroundId backgroundSelectedPreset();
int backgroundCount();
bool backgroundIsCustom();
// Soul: restore an optional custom chord from NVS (marks it as law); n=0
// faithfully restores a deliberately empty custom background.
void backgroundRestoreChord(const float* cents, int n);
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
// true exactly once per replayed phrase note. sourceCents points at its seed;
// playedCents drives the firefly after the phrase-wide transposition.
bool pollGhost(float* sourceCents, float* playedCents);
// the player's current preset index: ghosts play with it, while the
// background keeps its own per-note ORGAN timbre (a percussive player preset
// must not silently kill the drone or disturb the player's global filter)
void setPreset(int idx);

}  // namespace ambient
