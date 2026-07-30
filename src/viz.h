// Nocna łąka — wizualizacja muzyki na 240x135, zero tekstu podczas grania.
//
// Metafora spójna z tym, co słychać:
//   nuta      -> świetlik: pojawia się na wysokości dźwięku, świeci póki brzmi
//   koniec    -> świetlik opada w łąkę i zostaje jako przygaszona iskierka
//                (pamięć ogrodu duchów, wreszcie WIDOCZNA)
//   duch      -> gdy pudełko powtarza wspomnienie, iskierka rozbłyska i
//                unosi się na chwilę
//   machanie  -> snop iskier przy każdym dzwonku
//   tło       -> pulsujące pagórki na horyzoncie (akord tampury)
//   pogoda    -> gwiazdy mrugają w rytmie oddechu, ziemia jaśnieje z dźwiękiem
//   przechył  -> księżyc wędruje po niebie (jasność brzmienia = jasność nieba)
//
// Rysuje pełną klatkę przy każdym wywołaniu — wołać z draw() trybu przy
// ciągłym markDirty(); main i tak ogranicza do ~25 fps.
#pragma once
#include <M5GFX.h>

namespace viz {

// Sceny: te same nuty, wspomnienia i gesty — RÓŻNA kompozycja i ruch.
//   laka    — świetliki nad trawą, opadają w łąkę (horyzont)
//   kosmos  — planety krążą po orbitach wokół środka (radialna)
//   ocean   — ryby PŁYNĄ przez ekran, perły toną na dno (przepływ)
//   ognie   — nuty ROZSZERZAJĄ się jako wybuchy z żarem (ekspansja)
//   mandala — symetryczny wzór z brzmiących nut, obraca się (abstrakcja)
constexpr int kSceneCount = 5;
const char* sceneName(int idx);
void setScene(int idx);

void reset();

// zakres wysokości aktualnej skali (mapowanie nuta -> pozycja X/Y sceny);
// tryb woła to przy wejściu. foldMax01 = najwyższy stopień skali w oktawie
// (0..1, np. pentatonika 884/1200) — X zawija się do oktawy i rozciąga do
// tej wartości, żeby skala zajmowała pełną szerokość ekranu
void setPitchRange(float loCents, float hiCents, float foldMax01);

void noteOn(int id, float cents, float vel);
void noteOff(int id);
void chime(float cents, float vel);  // dzwonek z machania
void ghost(float cents);             // ogród właśnie zagrał wspomnienie
void voice(float cents, float lvl);  // śpiew na żywo: chłodna kometa
                                     // na wysokości głosu (gaśnie sama)
void setTilt(float norm);            // przechył boczny -1..1 (jasność)
void setDepth(float norm);           // przechył do/od siebie -1..1 (przestrzeń)

// duży napis na ~1.2 s (zmiana barwy itp.); nullptr nic nie robi
void toast(const char* text);

// ucho otwarte: pulsujący pierścień w rogu — uczciwy znak "pudełko słucha"
// i jedyna stała różnica między ŚPIEWEM a INSTRUMENTEM
void setListening(bool on);
// głośność głosu 0..1 — pierścień rośnie, gdy pudełko słyszy śpiew
void setVoiceLevel(float lvl);

void draw(M5Canvas& g);

}  // namespace viz
