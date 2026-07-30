# grajek

A pocket instrument that **cannot be played badly** — built by a dad for his
kid, on an **M5Stack Cardputer-ADV** (ESP32-S3, ~$60 off the shelf).

You shake it, tilt it, click it, sing into it — and something beautiful
comes out. Under the toy there is a serious synthesis engine: just
intonation in the spirit of Harry Partch, an aging tape loop, sympathetic
string resonators, and a heartbeat that follows *your* tempo instead of
imposing one.

## Why

It started as an experimental microtonal instrument and, iteration by
iteration ("where's the fun?", "too dark", "I hate toys that nag"),
converged on its real purpose: a first instrument for a child. These are
the design laws that survived the process:

1. **Impossible to play badly.** Pentatonic just intonation by default,
   equal-loudness key tracking, polyphony compensation, no failure states —
   any random key, any random pair of keys, sounds intentional.
2. **Cat, not Furby.** The box never solicits play. Every sound traces back
   to a human act: pick it up and it purrs, play and it remembers, leave it
   and it goes quiet. The microphone runs only while a key is held; nothing
   is ever stored.
3. **Tempo is discovered, not declared.** There is no BPM anywhere. Play a
   few even notes and a quiet heart joins in *your* time (a phase-locked
   loop on your key presses); drift into rubato and it lets go.
4. **Discovery instead of configuration.** Every key always plays — there
   are no special keys among the playing keys, ever. The default world
   (pentatonic + chimes) is safe for a toddler; deeper scales simply sit
   further down the settings list, ordered happy-to-strange, waiting for an
   older kid to find them. No age switch, no parent manual.
5. **Memory is the soul.** It saves what you taught it — your background
   chord, your scale, the notes you played — and greets you the next day
   with one of them.

The gestures on top: **shake** the box and it rattles a melody in scale —
every swing chimes one note, the waving direction steers it up or down,
the swing energy sets the loudness; **tilt** it and everything darkens or
brightens (slow and smooth, like turning the box away from the light);
**sing** into the ear and the tape returns you, darker each pass, haloed
by strings tuned to pure intervals.

## Status

- **Laptop rig (macOS)**: fully playable, this is where everything is
  designed and measured — see below.
- **Device firmware**: plays on real hardware — the whole chain is ported
  (engine → strings → lo-fi tape → reverb, ambient brain, IMU gestures)
  with five visualization scenes and a menu-less UI.
- **Microphone on the device: not working yet**, and the reason is worth
  writing down (see [The ear](#the-ear-microphone-on-the-adv--open-problem)).
  Everything else on this list is verified by playing it.

---

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
  input/            keyboard (TCA8418 polled over I2C) + BtnGO
  modes/            INSTRUMENT (play), SPIEW (play + open ear), USTAWIENIA
  viz.cpp           five visualization scenes — same music, different worlds
  duet.cpp          live pitch tracking: the box hums along with a singer
  settings.cpp      shared musical state (scale/timbre/octave/scene), in NVS
  main.cpp          main loop; boots straight into playing
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

**Toss simulator** (a laptop-only toy — on the device the throw gave way to
the shake-rattle gesture): `SPACE` throws the whole music into a rising glissando
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

### Playing it: there is no menu

The box **boots straight into playing**, and the keyboard is *only* music:
**all 56 keys play**, in every mode, forever. Column = scale step, bottom
row = the lowest interval; no key is ever special, so a toddler cannot get
lost and cannot find a wrong key.

Everything else hangs off the one side button:

| Gesture | What it does |
|---|---|
| any key | plays (column = scale step, bottom row = lowest) |
| **short BtnGO** | toggles playing ⇄ settings screen |
| **hold BtnGO** | next timbre (repeats every 0.7 s, the name flashes) |
| **shake** | the *wind of memories* — each swing plays back a note the child really played |
| **tilt sideways** | brightness: darkens or opens the sound (filter) |
| **tilt toward / away** | depth: the sound moves into the reverb and echo, or comes close and dry |

The settings screen (short BtnGO) has six rows, each cycled by its digit
and persisted in NVS: **scale** (ordered happy → strange: pentatonic JI,
just major, 12-EDO, 19-EDO, 31-EDO, 11-limit Partch), **timbre**,
**octave**, **background drone**, **visualization scene**, **ear**.

Two design laws are enforced here rather than explained: the shake plays
*memories* instead of a scale, so the keyboard and the gesture feed each
other instead of competing; and both tilts are computed from a low-passed
gravity vector and smoothed over ~0.4 s, so shaking the box never jerks
the filter and a child's unsteady hands do not wobble the sound.

### The screen: five worlds, one music

The same notes, memories and gestures drive five scenes that differ in
*composition and motion*, not decoration:

- **laka** (meadow) — fireflies over grass; a note that ends drifts down
  and stays as a glowing seed. **This is the ghost garden made visible**:
  when the box replays a remembered note after a silence, its seed flares.
- **kosmos** — radial: notes **orbit** a pulsing core (higher = further and
  faster), memories are stars in an outer ring; sideways tilt spins the
  whole system, depth tilt flattens the orbital plane.
- **ocean** — flow: fish **swim across** the screen and wrap around, light
  shafts lean with the tilt, memories sink to the sand as pearls, the
  shake sends bubbles up.
- **ognie** (fireworks) — expansion: each note **blooms as a growing ring**
  and falls back as embers; near-empty sky, all attention on the notes.
- **mandala** — abstraction: sounding notes form a symmetric rotating
  pattern; depth tilt changes the symmetry from 4 to 9 arms.

### Sound chain on the device

Same portable modules as the host: engine → sympathetic strings → echo tape
(3 s at 16 kHz behind a lo-fi rate bridge — no PSRAM, and the aging tape
loves the darkening anyway) → reverb → a hard child-ear ceiling. The
ambient brain (background chord, weather, ghost garden) runs from boot.
Deferred to v2: FREEZE/looper floor (RAM audit on real hardware first),
psychoacoustic bass for the tiny speaker, and the ear (below).

### The ear (microphone on the ADV) — open problem

`SPIEW` mode is written and waiting: the voice would enter the chain where
the synth does, a voice-gated duck keeps the mic→speaker loop from closing
(sing → the music steps aside and the tape records you; stop → the box
answers with your own voice), and `duet.cpp` tracks the singer's pitch live
to hum a consonant scale tone below it. It all works on the laptop.

On the device the codec's ADC stays silent, and here is what a night of
measuring established, in case it saves someone else the same night:

- The **hardware is fine** — the factory UserDemo records happily on the
  same unit, and the pin (ASDOUT → G46) matches the schematic.
- **Not a register problem.** Faithful transcriptions of `esp-bsp`,
  `esp-adf` *and* the official M5Unified sequences all leave the line idle,
  and so does the exact register set snorted from the factory firmware
  while it was recording.
- **Two real findings on the way:** the ES8311 only latches its ADC
  configuration on a *clean clock start* (write registers with I2S
  disabled, then enable — writes while BCLK runs do nothing), and the
  `i2s_std` driver in full-duplex mode does not route the DIN pad to the
  RX unit at all (a GPIO-matrix loopback of our own TX proves the RX side
  works; the fix is an explicit `esp_rom_gpio_connect_in_signal`).
- **Diagnosis:** M5Unified's `Mic_Class::mic_task` drives the I2S unit
  *below* the driver — raw clock dividers (`i2s_ll_rx_set_raw_clk_div`),
  its own `rx_bck_div_num`, an explicit RX reset. That is what the ADC
  apparently needs on this board, and nothing assembled from the public
  `i2s_std` API reproduces it.
- **Plan:** stop re-deriving it and use `M5.Mic` itself inside the
  listening windows — which fits the walkie-talkie design anyway, since
  the speaker is meant to be quiet while the child sings.

The firmware keeps a serial diagnostic console for this hunt (single
letters over `Serial`: dump codec registers, probe the RX stream, GPIO
loopback, relatch the clocks, `Wrrvv` writes any register) — see
`hal::audioDiag` in `src/hal/audio_out.cpp`.

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
  audio task runs on **core 0** (UI owns core 1), DMA buffers 4×96 frames,
  synthesis in 96-sample blocks @ 48 kHz. The ES8311 setup follows
  `espressif/esp-bsp` for the duplex case and official M5Unified
  (`_speaker_enabled_cb_cardputer_adv`) for playback only.
- The engine never allocates after `init()` and is controlled exclusively
  through an SPSC queue — no mutexes anywhere near the audio path.
- **The keyboard is polled, not interrupt-driven.** M5Cardputer's TCA8418
  reader can wedge (INT low with the flag already cleared → the keyboard
  goes dead while the rest of the box keeps humming, which is exactly how
  it failed in a child's hands). We read the controller's event FIFO over
  I2C every pass instead, and do not call `M5Cardputer.update()` at all so
  nothing else drains that FIFO.
- Isomorphic grid: column = +1 scale step, row ≈ a fourth (EDO) or +octave
  (JI); the geometry follows from the scale (`ga_scales.cpp`).

## What to check after flashing

1. It boots **playing** (a quiet drone) and every key sounds. If rows come
   out upside down, swap `3 - row` in `mode_instrument.cpp`.
2. Keys stay clean **with no clicks/dropouts** while the UI is stressed —
   hold several keys, shake, switch scenes.
3. Short BtnGO opens settings and returns; holding it while playing cycles
   the timbre; every choice survives a power cycle (NVS).
4. Shaking replays remembered notes (one swing = one note, not a burst);
   both tilts respond smoothly — if an axis feels swapped, exchange
   `gravX_` / `gravY_` in `imuStep()`.
5. The 3.5 mm jack mutes the speaker (purely hardware — should just work).
   It is also the way to record the box cleanly: the tiny speaker does not
   do it justice.

## Roadmap

1. ✅ A playing core on the PC (WAV + live playing) and the ESP32 firmware
   with INSTRUMENT, gestures and the visualization scenes
2. The ear: switch the listening windows to `M5.Mic` (see
   [The ear](#the-ear-microphone-on-the-adv--open-problem)) — that unlocks
   SPIEW on the device, and the mic looper after it
3. LOOP — microphone looper. The layered looper core is DONE and playable on
   the host (see above); what remains is the input path above. No PSRAM:
   ~200 KB of RAM for loops → mono 16-bit @ 24 kHz ≈ 4 s of loop in RAM
   (2 buffers, no undo), or stream to microSD.
4. DRONE — generative mode; "lying flat" detection hooks into the existing
   BMI270 path.
5. IR CONDUCTOR — NEC sequencer on G44 (RMT). Learning codes from a remote
   needs an IR receiver the ADV **does not have** — so: a protocol database
   plus optional learning via a Grove-connected receiver (G1/G2).
