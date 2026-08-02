#include "keys.h"

#include <Arduino.h>
#include <M5Cardputer.h>

#include "../hal/board_pins.h"

namespace {

uint64_t s_cur = 0;
uint64_t s_releasePending = 0;

bool s_goLast = false;
uint32_t s_goChangedAt = 0;
bool s_goPressEdge = false;
bool s_goReleaseEdge = false;

// TCA8418: rejestry zdarzeń (Adafruit_TCA8418_registers.h)
constexpr uint8_t kRegIntStat = 0x02;   // INT_STAT (bit0 = K_INT)
constexpr uint8_t kRegKeyLckEc = 0x03;  // licznik zdarzeń w FIFO (dolne bity)
constexpr uint8_t kRegKeyEventA = 0x04; // odczyt zdejmuje zdarzenie z FIFO
constexpr uint32_t kI2cFreq = 400000;
constexpr uint8_t kOverflowInt = 0x08;

inline int bitIndex(int col, int row) { return row * 14 + col; }

// remap surowego zdarzenia matrycy 7x8 na siatkę 4x14 — 1:1 z biblioteki
// M5Cardputer (KeyboardReader/TCA8418.cpp), żeby współrzędne się zgadzały
void remapEvent(uint8_t raw, int& col, int& row, bool& down) {
  down = (raw & 0x80) != 0;
  uint16_t b = (uint16_t)(raw & 0x7F) - 1;
  const int rawRow = b / 10;
  const int rawCol = b % 10;
  col = rawRow * 2 + (rawCol > 3 ? 1 : 0);
  row = rawCol % 4;
}

}  // namespace

namespace input {

void keysInit() {
  pinMode(hal::kPinBtnGo, INPUT);  // external 10K pull-up on the board
  s_cur = 0;
  // Konfigurację TCA8418 (matryca 7x8, flush) zrobił M5Cardputer.begin().
  // Zdarzenia czytamy SAMI, pollingiem po I2C — czytnik biblioteki działa
  // na przerwaniu z flagą i potrafił się zakleszczyć (INT nisko + flaga
  // skasowana = klawiatura martwa na zawsze, a reszta pudełka żyje).
  // main NIE woła już M5Cardputer.update(), żeby biblioteka nie podkradała
  // zdarzeń z tego samego FIFO.
}

int keysPoll(KeyEvent* out, int maxEvents) {
  // próbka GO raz na przebieg — krawędzie żyją do następnego keysPoll
  s_goPressEdge = false;
  s_goReleaseEdge = false;
  const bool goDown = digitalRead(hal::kPinBtnGo) == LOW;
  const uint32_t now = millis();
  if (goDown != s_goLast && (now - s_goChangedAt) > 30) {
    s_goLast = goDown;
    s_goChangedAt = now;
    if (goDown) s_goPressEdge = true;
    else s_goReleaseEdge = true;
  }

  int n = 0;
  const uint8_t intStat =
      M5.In_I2C.readRegister8(hal::kTca8418Addr, kRegIntStat, kI2cFreq);
  if (intStat & kOverflowInt) {
    // Hardware FIFO is only ten events deep. A lost key-up is worse than a
    // short panic. Surviving events are not trustworthy either: an old key-down
    // may remain while its later key-up was the event that overflow discarded.
    // Release everything we believed held and discard the whole damaged FIFO;
    // physical keys can be released and pressed again immediately.
    s_releasePending |= s_cur;
    s_cur = 0;
    uint8_t discard = M5.In_I2C.readRegister8(
                          hal::kTca8418Addr, kRegKeyLckEc, kI2cFreq) &
                      0x0F;
    while (discard-- > 0)
      M5.In_I2C.readRegister8(hal::kTca8418Addr, kRegKeyEventA, kI2cFreq);
    M5.In_I2C.writeRegister8(hal::kTca8418Addr, kRegIntStat, 0x0F, kI2cFreq);
    Serial.println("grajek: klawiatura FIFO overflow — panic/recovery");
  }
  for (int bit = 0; bit < 56 && n < maxEvents; ++bit) {
    const uint64_t mask = (uint64_t)1 << bit;
    if (!(s_releasePending & mask)) continue;
    s_releasePending &= ~mask;
    s_cur &= ~mask;
    out[n++] = {(uint8_t)(bit % 14), (uint8_t)(bit / 14), false};
  }

  // Do not reinterpret any event from an overflowed epoch. A new event that
  // arrived during the drain remains in hardware and is picked up next pass.
  if (intStat & kOverflowInt) return n;

  uint8_t ec =
      M5.In_I2C.readRegister8(hal::kTca8418Addr, kRegKeyLckEc, kI2cFreq) & 0x0F;
  const bool hadEvents = ec > 0;
  while (ec-- > 0 && n < maxEvents) {
    const uint8_t raw =
        M5.In_I2C.readRegister8(hal::kTca8418Addr, kRegKeyEventA, kI2cFreq);
    if ((raw & 0x7F) == 0) break;  // pusty odczyt — FIFO się rozjechało
    int col, row;
    bool down;
    remapEvent(raw, col, row, down);
    if (col < 0 || col >= 14 || row < 0 || row >= 4) continue;
    const uint64_t m = (uint64_t)1 << bitIndex(col, row);
    if (down) s_cur |= m;
    else s_cur &= ~m;
    out[n].col = (uint8_t)col;
    out[n].row = (uint8_t)row;
    out[n].down = down;
    ++n;
  }
  if (intStat || hadEvents)  // sprzątnij flagi, także po odczycie bez INT
    M5.In_I2C.writeRegister8(hal::kTca8418Addr, kRegIntStat, 0x0F, kI2cFreq);
  return n;
}

bool goReleased() { return s_goReleaseEdge; }
bool goPressed() { return s_goPressEdge; }

uint32_t goHeldMs() {
  return s_goLast ? millis() - s_goChangedAt : 0;
}

}  // namespace input
