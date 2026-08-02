// M5Stack Cardputer-ADV pins and addresses.
//
// Everything verified against official sources (2026-07):
//  - M5Unified 0.2.10 src/M5Unified.cpp (case board_M5CardputerADV)
//  - M5GFX src/M5GFX.cpp (CardputerADV autodetect)
//  - M5Cardputer-UserDemo, branch CardputerADV, main/hal/hal_config.h
//  - schematic Sch_M5CardputerAdv_v1.0 (m5stack-doc.oss-cn-shenzhen.aliyuncs.com)
//
// NOTE — differences from common assumptions:
//  - There is NO power-hold pin. G38 is the LCD backlight. Power is held by
//    a mechanical slide switch (SW1) in the battery path — firmware sets nothing.
//  - There is NO MCLK line to the codec: the ES8311 runs in MCLK=BCLK mode
//    (register 0x01 = 0xB5).
//  - There is NO GPIO enabling the NS4150B amp: the 3.5mm jack mutes it in
//    hardware, ES8311 registers gate it in software.
//  - The keyboard is a TCA8418 controller on I2C (7x8 matrix remapped to
//    4x14), not a GPIO scan like the original Cardputer.
#pragma once
#include <stdint.h>

namespace hal {

// --- audio playback: ES8311 over I2S (ESP32-S3 = master) ---
constexpr int kPinI2sBclk = 41;  // ES8311 SCLK
constexpr int kPinI2sWs   = 43;  // ES8311 LRCK
constexpr int kPinI2sDout = 42;  // ESP32 -> ES8311 DSDIN (playback)

// --- internal I2C bus (codec + IMU + keyboard) ---
constexpr int kPinI2cSda = 8;
constexpr int kPinI2cScl = 9;
constexpr uint8_t kEs8311Addr  = 0x18;
constexpr uint8_t kBmi270Addr  = 0x69;
constexpr uint8_t kTca8418Addr = 0x34;
constexpr int kPinKeyboardInt  = 11;  // TCA8418 INT (active low)

// --- other peripherals ---
constexpr int kPinIrTx    = 44;  // IR transmitter (through a 22R resistor)
constexpr int kPinBtnGo   = 0;   // BtnGO / boot, external 10K pull-up, active low
constexpr int kPinRgbLed  = 21;  // WS2812
constexpr int kPinBatAdc  = 10;  // ADC1, 1:2 divider
constexpr int kPinSdCs    = 12;  // microSD on SPI: SCK=40, MOSI=14, MISO=39
constexpr int kPinSdSck   = 40;
constexpr int kPinSdMosi  = 14;
constexpr int kPinSdMiso  = 39;

// LCD (ST7789 240x135): handled by M5GFX autodetect —
// RST=33, MOSI=35, SCLK=36, DC=34, CS=37, BL=38 (PWM), offset 52/40, rotation 1.

// EXT bus (future LoRa SX1262 + GNSS AT6668 cap):
// RESET=3, INT/DIO1=4, BUSY=6, CS=5, SCK=40, MOSI=14, MISO=39 (shared with SD),
// GPS TX->ESP32 RX=15, GPS RX<-ESP32 TX=13, I2C: SCL=9, SDA=8.

}  // namespace hal
