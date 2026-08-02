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
bool save();  // zapisuje tylko pola zmienione od ostatniego zapisu/odczytu

ga::ScaleId scale();
int preset();        // indeks barwy (kNumTimbrePresets)
int octave();        // indeks do {55, 110, 220, 440} Hz
float baseHz();      // częstotliwość centrum wynikająca z oktawy
bool backgroundOn();
int vizScene();      // scena wizualizacji (viz::kSceneCount)

const char* presetName(int idx);

// Zmiany cykliczne są natychmiastowe w RAM; NVS dostaje końcowy wybór przy
// wyjściu z ustawień / puszczeniu długiego GO, nie każdy krok karuzeli.
void cycleScale();
void cyclePreset();
void cycleOctave();
void toggleBackground();
void cycleVizScene();

// wciska aktualny stan w silnik + struny + ambient (po zmianie i na starcie)
void applyToEngine(ga::Engine& e);

}  // namespace settings
