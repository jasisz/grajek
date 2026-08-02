// Input: the 4x14 keyboard (TCA8418, polled directly over I2C) + BtnGO.
// The M5Cardputer library reader is deliberately NOT used — its
// interrupt-based path could wedge and freeze the keyboard forever.
// Turns keyboard state into down/up events with raw coordinates
// (col 0..13, row 0..3, row 0 = the top physical row).
#pragma once
#include <stdint.h>

namespace input {

struct KeyEvent {
  uint8_t col;
  uint8_t row;
  bool down;
};

// Enough to synthesize releases for every held key if the TCA8418's own
// 10-entry hardware FIFO overflows during a blocking operation.
constexpr int kMaxKeyEvents = 56;

void keysInit();

// Call once per main-loop pass. main.cpp deliberately never calls
// M5Cardputer.update() — the library would drain the same event FIFO.
// Returns the number of events written to out (at most maxEvents).
int keysPoll(KeyEvent* out, int maxEvents);

// BtnGO (G0), debounced. Sampled by keysPoll() — the edge stays valid for
// one loop pass (main calls keysPoll once per pass).
bool goPressed();   // true exactly once, on press
bool goReleased();  // true exactly once, on release

// ile ms GO jest trzymany (0 gdy puszczony) — przytrzymanie = akcja trybu
uint32_t goHeldMs();

}  // namespace input
