#include "keys.h"

#include <Arduino.h>
#include <M5Cardputer.h>

#include "../hal/board_pins.h"

namespace {

uint64_t s_prev = 0;
uint64_t s_cur = 0;

bool s_goLast = false;
uint32_t s_goChangedAt = 0;

inline int bitIndex(int col, int row) { return row * 14 + col; }

}  // namespace

namespace input {

void keysInit() {
  pinMode(hal::kPinBtnGo, INPUT);  // external 10K pull-up on the board
  s_prev = s_cur = 0;
}

int keysPoll(KeyEvent* out, int maxEvents) {
  s_prev = s_cur;
  s_cur = 0;
  for (auto& p : M5Cardputer.Keyboard.keyList()) {
    if (p.x >= 0 && p.x < 14 && p.y >= 0 && p.y < 4)
      s_cur |= (uint64_t)1 << bitIndex(p.x, p.y);
  }
  int n = 0;
  const uint64_t changed = s_prev ^ s_cur;
  if (!changed) return 0;
  for (int row = 0; row < 4 && n < maxEvents; ++row) {
    for (int col = 0; col < 14 && n < maxEvents; ++col) {
      const uint64_t m = (uint64_t)1 << bitIndex(col, row);
      if (changed & m) {
        out[n].col = (uint8_t)col;
        out[n].row = (uint8_t)row;
        out[n].down = (s_cur & m) != 0;
        ++n;
      }
    }
  }
  return n;
}

bool keyHeld(int col, int row) {
  if (col < 0 || col >= 14 || row < 0 || row >= 4) return false;
  return (s_cur >> bitIndex(col, row)) & 1;
}

bool goPressed() {
  const bool down = digitalRead(hal::kPinBtnGo) == LOW;
  const uint32_t now = millis();
  if (down != s_goLast && (now - s_goChangedAt) > 30) {
    s_goLast = down;
    s_goChangedAt = now;
    if (down) return true;
  }
  return false;
}

}  // namespace input
