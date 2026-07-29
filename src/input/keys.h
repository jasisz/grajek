// Input: the 4x14 keyboard (TCA8418 via M5Cardputer.Keyboard) + BtnGO.
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

// Enough for every key changing at once — poll with this size and event
// overflow is physically impossible.
constexpr int kMaxKeyEvents = 56;

void keysInit();

// Call once per main-loop pass (after M5Cardputer.update()).
// Returns the number of events written to out (at most maxEvents).
int keysPoll(KeyEvent* out, int maxEvents);

bool keyHeld(int col, int row);

// BtnGO (G0) with debounce; true exactly once per press.
bool goPressed();

// How long BtnGO has been held, in ms (0 when up) — the parent gesture
// (long-press in the menu) cycles the age tier.
uint32_t goHeldMs();

}  // namespace input
