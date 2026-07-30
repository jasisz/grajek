// Wspólny stan muzyczny: skala, barwa, oktawa, tło. Jedno źródło prawdy —
// czyta go tryb INSTRUMENT, zmienia ekran USTAWIENIA i przytrzymany GO.
//
// Bez trybów wieku: stan domyślny (pentatonika + CHIME) jest bezpieczny dla
// malucha, a głębsze skale są po prostu dalej w cyklu — od wesołych do
// dziwnych. Odkrywanie zamiast konfiguracji. Wybory trwają w NVS.
#pragma once
#include "ga_engine.h"
#include "ga_scales.h"
#include "ga_voice.h"

namespace settings {

void load();  // raz w setup(), przed applyToEngine

ga::ScaleId scale();
int preset();        // indeks barwy (kNumTimbrePresets)
int octave();        // indeks do {55, 110, 220, 440} Hz
float baseHz();      // częstotliwość centrum wynikająca z oktawy
bool backgroundOn();
int vizScene();      // scena wizualizacji (viz::kSceneCount)
bool singOn();       // tryb ŚPIEW: ucho otwarte podczas grania

const char* presetName(int idx);

// zmiany cykliczne; każda od razu zapisuje się w NVS
void cycleScale();
void cyclePreset();
void cycleOctave();
void toggleBackground();
void cycleVizScene();
void toggleSing();

// wciska aktualny stan w silnik + struny + ambient (po zmianie i na starcie)
void applyToEngine(ga::Engine& e);

}  // namespace settings
