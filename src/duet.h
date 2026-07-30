// DUET: pudełko nuci razem ze śpiewającym (port z hosta, live_main.cpp).
// Śledzenie wysokości głosu z okna ucha (autokorelacja, 80-600 Hz), towarzysz
// = jeden miękki głos PURE, który ślizga się legato na spółbrzmiący stopień
// skali PONIŻEJ śpiewu (tercja, awaryjnie kwarta). Głos człowieka zostaje
// nietknięty — pudełko jest drugim śpiewakiem, nie imitacją.
#pragma once
#include "ga_engine.h"

namespace duet {

void reset();                 // świeże śledzenie (wejście w tryb ŚPIEW)
void tick(ga::Engine& e);     // wołać co przebieg pętli przy otwartym uchu
void humOff(ga::Engine& e);   // ucisz towarzysza (wyjście z trybu)

}  // namespace duet
