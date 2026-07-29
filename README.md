# grajek

An experimental musical instrument on the **M5Stack Cardputer-ADV** (ESP32-S3).
Priority: low latency and smooth sound — this is a live instrument.

## Layout

```
lib/grajek_audio/   synthesis engine — pure C++17, ZERO hardware dependencies
                    (sine-partial oscillators, ADSR, SVF filter,
                     scales: 12/19/31-EDO + 11-limit just intonation,
                     lock-free event queue; the same code runs on the ESP32
                     and on a laptop)
host/               PC targets for shaping the sound without flashing
src/                ESP32 firmware (PlatformIO / Arduino core 3.x):
  hal/              I2S + ES8311, pins (board_pins.h — all verified)
  input/            keyboard (TCA8418 via M5Cardputer) + BtnGO
  modes/            modes: INSTRUMENT (done), LOOP / DRONE / IR (stubs)
  main.cpp          menu + main loop
```

## Playing on the laptop (no hardware needed)

```bash
cd host
make play        # grajek_live — REAL-TIME playing via CoreAudio (macOS)
make run         # grajek_host — renders demo WAVs into host/out/
afplay out/01_single_note.wav
```

`grajek_live`: 4 laptop keyboard rows = 4 instrument grid rows (the bottom
`zxcv…` row plays lowest). A terminal has no key-up events, so a note fades
0.6 s after release (auto-repeat sustains it), and **SHIFT+L toggles LATCH
mode**: a key press turns a note on permanently, a second press turns it
off — that is how you build drones. `TAB` scale, `` ` `` timbre, `BACKSPACE`
octave, arrows = IMU stand-in (left/right bend, up/down filter), `ENTER`
panic + re-center, `ESC` quit.

**Toss simulator** (prototype of the device's throw gesture — on hardware the
IMU will drive it): `SPACE` throws the whole music into a rising glissando
(synth bend + loop varispeed together), arrows **during flight** add spin
(right = land upward/otonal, left = land downward/utonal; more spin = flight
warble + a dizzy landing), `SPACE` again catches: flight time picks the rung
of the JI ladder (9/8, 5/4, 3/2, 7/4, 2/1) and everything lands re-rooted on
the new tonal center. `SHIFT+X` = fumble: tape-dive crash, the top loop layer
is lost.

**Looper** (`lib/grajek_audio/ga_looper.*` — the same core the device LOOP
mode will use, fed there by the microphone): `SHIFT+R` starts the first
recording, pressed again it closes the loop and defines its length; from then
on `SHIFT+R` toggles overdub (each take commits one layer). `SHIFT+P`
play/stop, `SHIFT+U` undo the last layer, `SHIFT+C` clear, `SHIFT+[` /
`SHIFT+]` loop playback volume (default 80% — leaves headroom to play over
the loop). Record a phrase, let it circle, layer on top — panic (`ENTER`)
silences the synth but keeps the loop running.

The engine applies equal-power polyphony compensation (1/sqrt(voices),
slowly smoothed), so a held chord sits at roughly the level of a single
note instead of squashing into the clipper.

## Flashing the Cardputer-ADV

PlatformIO lives in a project-local uv environment (`.venv`, gitignored):

```bash
uv venv .venv && uv pip install --python .venv/bin/python platformio  # once
.venv/bin/pio run -e cardputer-adv -t upload
.venv/bin/pio device monitor
```

On the device: menu → keys `1`–`4` pick a mode, **BtnGO returns to the
menu**. In INSTRUMENT mode the leftmost keyboard column is the control column
(top to bottom: scale, timbre, IMU tilt role, octave); the remaining 13×4
keys play.

**Device v1 sound chain** (same portable modules as the host): engine →
sympathetic strings → echo tape (3 s at 16 kHz behind a lo-fi rate bridge —
no PSRAM, and the aging tape loves the darkening anyway) → reverb. The
ambient brain (background chord, weather, ghost garden) runs from boot, in
the menu too. Gestures: **toss** is always armed — free fall lifts the music
in a glissando, the catch lands it re-rooted on a JI rung (flight time =
rung, spin direction = up/down); **hold `ctrl` + grid keys** picks the
background chord (tap `ctrl` = octave). Deferred to v2: FREEZE/looper floor
(RAM audit on real hardware first), the mic path, psychoacoustic bass for
the tiny speaker.

## Hardware — facts verified against official sources

Sources: M5Unified 0.2.10, M5GFX (ADV autodetect), M5Cardputer-UserDemo
(branch `CardputerADV`), schematic `Sch_M5CardputerAdv_v1.0`. Details and
quotes in `src/hal/board_pins.h`.

| Part | Configuration |
|---|---|
| ES8311 (codec) | I2C `0x18` on G8/G9; I2S: BCLK=G41, WS=G43, DOUT=G42, DIN(mic)=G46; **no MCLK** — MCLK=BCLK mode (reg 0x01=0xB5) |
| NS4150B amp | **no GPIO enable** — muted in hardware by an inserted 3.5 mm jack |
| Microphone | analog MEMS → codec MIC1P/N input (not PDM!) |
| Keyboard | **TCA8418** controller over I2C (`0x34`), INT=G11 — not a GPIO scan |
| IMU BMI270 | I2C `0x69` |
| LCD ST7789 | handled by M5GFX autodetect; backlight = **G38** |
| Power | mechanical slide switch — **there is no power-hold pin** (G38 is NOT power!) |
| IR / LED / SD | IR TX=G44, WS2812=G21, SD CS=G12 (SPI G40/G14/G39), battery ADC=G10 |
| EXT (LoRa cap) | CS=G5, RST=G3, BUSY=G6, DIO1=G4, SPI shared with SD; GPS UART: RX=G15, TX=G13 |

## Architecture decisions

- **Own I2S path instead of `M5.Speaker`** — skips the M5Unified mixer; the
  audio task runs on **core 0** (UI owns core 1), DMA buffers 4×64 frames ≈
  **5.3 ms**, synthesis in 64-sample blocks @ 48 kHz. The ES8311 register
  sequence is taken 1:1 from official M5Unified
  (`_speaker_enabled_cb_cardputer_adv`).
- The engine never allocates after `init()` and is controlled exclusively
  through an SPSC queue — no mutexes anywhere near the audio path.
- Isomorphic grid: column = +1 scale step, row ≈ a fourth (EDO) or +octave
  (JI); the geometry follows from the scale (`ga_scales.cpp`).

## What to test on hardware (step 1)

1. After flashing: the menu appears and key `1` enters INSTRUMENT.
2. Keys play a clean tone **with no clicks/dropouts** (while playing, stress
   the UI — hold several keys, cycle scales).
3. The control column switches: scale (`` ` ``), timbre (tab), IMU role (fn),
   octave (ctrl) — and the LCD labels match the physical rows.
   If rows come out flipped → swap `3 - row` in `mode_instrument.cpp`.
4. Tilting bends the pitch (BEND role); if the wrong axis responds → swap
   `ax` for `ay` in `applyImu()`.
5. The 3.5 mm jack mutes the speaker (purely hardware — should just work).
6. BtnGO: back to menu, silence, re-entering works.

## Roadmap

1. ✅ A playing core on the PC (WAV + live playing) and the ESP32 skeleton
   with INSTRUMENT
2. INSTRUMENT polish (interval hints on the grid, timbre tuning)
3. LOOP — microphone looper. The layered looper core is DONE and playable on
   the host (see above); the device work that remains is the audio input
   path. Note: the official demo never runs the codec ADC and DAC at once;
   plan: full duplex on a single I2S controller (shared BCLK/WS, TX=G42 +
   RX=G46) with a merged codec register setup — needs hardware verification;
   fallback: switch between record and playback. No PSRAM: ~200 KB of RAM
   for loops → mono 16-bit @ 24 kHz ≈ 4 s of loop in RAM (2 buffers, no
   undo), or stream to microSD.
4. DRONE — generative mode; "lying flat" detection hooks into the existing
   BMI270 path.
5. IR CONDUCTOR — NEC sequencer on G44 (RMT). Learning codes from a remote
   needs an IR receiver the ADV **does not have** — so: a protocol database
   plus optional learning via a Grove-connected receiver (G1/G2).
