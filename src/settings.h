// Wspólny stan muzyczny: skala, barwa, oktawa, tło, scena i opcjonalny ślizg.
// Jedno źródło prawdy — czyta go INSTRUMENT, zmienia USTAWIENIA i GO.
//
// Bez trybów wieku: stan domyślny (pentatonika + CHIME) jest bezpieczny dla
// malucha, a głębsze skale są po prostu dalej w cyklu — od wesołych do
// dziwnych. Odkrywanie zamiast konfiguracji. Wybory trwają w NVS.
#pragma once
#include "ga_engine.h"
#include "ga_scales.h"
#include "ga_voice.h"
#include "gk_background.h"

namespace settings {

void load();  // raz w setup(), przed applyToEngine
bool save();  // zapisuje tylko pola zmienione od ostatniego zapisu/odczytu

ga::ScaleId scale();
int preset();        // indeks barwy (kNumTimbrePresets)
int octave();        // indeks do {55, 110, 220, 440} Hz
float baseHz();      // częstotliwość centrum wynikająca z oktawy
gk::BackgroundId backgroundPreset();
int vizScene();      // scena wizualizacji (viz::kSceneCount)
// Ślizg kolejnych nut w tym samym rzędzie: wyłączony, krótkie lądowanie
// albo długi, wyraźnie śpiewający portament.
enum class GlideMode : uint8_t { Off = 0, Soft, Strong };
GlideMode glide();

// Where the sound is going. The whole output chain — the psychoacoustic bass,
// the 180 Hz high-pass, the presence shelf — is tuned for a 3 cm driver that
// cannot reproduce a low fundamental. On a real speaker through the jack that
// same tuning is an amputation: it removes bass the speaker CAN play and adds
// a presence lift it does not need.
enum class OutputMode : uint8_t { Speaker = 0, Jack };
OutputMode output();

const char* presetName(int idx);

// Zmiany cykliczne są natychmiastowe w RAM; NVS dostaje końcowy wybór przy
// wyjściu z ustawień / puszczeniu długiego GO, nie każdy krok karuzeli.
void cycleScale();
void cyclePreset();
void cycleOctave();
void cycleBackground();
void cycleVizScene();
void cycleGlide();
void cycleOutput();

// wciska aktualny stan w silnik + struny + ambient (po zmianie i na starcie)
void applyToEngine(ga::Engine& e);

}  // namespace settings
