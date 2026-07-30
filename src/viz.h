// Music made visible on 240x135 — five scenes, one world, zero text while
// playing. Every scene draws the same shared state (sounding notes, the
// memory seeds, ghosts, the background chord, the weather's breath) with a
// different composition and motion — see the scene list below.
//
// The shared metaphor:
//   note      -> a being appears at the pitch's position, alive while it rings
//   note ends -> it settles into the ground as a dim seed (the ghost
//                garden's memory, finally VISIBLE)
//   ghost     -> when the box replays a memory, its seed flares
//   shake     -> a burst of sparks with every chime
//   background-> pulsing shapes tied to the tampura chord
//   weather   -> stars twinkle with the breath, the ground brightens with sound
//   tilt      -> TWO axes: sideways = brightness (the moon wanders / the
//                system spins), toward/away = depth (orbits flatten, light
//                shafts lean, the mandala changes symmetry)
//
// Draws a full frame per call — call from the mode's draw() with a constant
// markDirty(); main caps it at ~25 fps anyway.
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
