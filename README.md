# grajek

A pocket instrument that **cannot be played badly** — built by a dad for his
kid, on an **M5Stack Cardputer-ADV** (ESP32-S3, ~$60 off the shelf).

https://github.com/user-attachments/assets/ff5505d3-2069-4a1e-8fab-654272a3c69c

You shake it, tilt it, click it — and something beautiful comes out.
Under the toy there is a serious synthesis engine: just
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
   and it goes quiet.
3. **Tempo is discovered, not declared.** There is no BPM anywhere. Play a
   few even notes and a quiet heart joins in *your* time (a phase-locked
   loop on your key presses); drift into rubato and it lets go.
4. **Discovery instead of configuration.** While playing, every grid key is
   music — there are no special keys among the playing keys. The default world
   (pentatonic + chimes) is safe for a toddler; deeper scales simply sit
   further down the settings list, ordered happy-to-strange, waiting for an
   older kid to find them. No age switch, no parent manual.
5. **Memory is the soul.** At natural pauses it saves what you taught it —
   your settings and short phrases with their timing — then greets you the
   next day with one remembered note.

The gestures on top: **shake** the box and it rattles the child's own
remembered phrases — every swing releases one short musical thought with
the child's rhythm intact, the waving direction steers the occasional
whole-phrase transposition up or down, and the swing energy sets the
loudness; **tilt** it and everything darkens or brightens (slow and smooth,
like turning the box away from the light); **lay it face-down** after
playing and the day's phrases sing themselves to sleep.

## Status

- **Laptop rig (macOS)**: fully playable, this is where everything is
  designed and measured — see below.
- **Device firmware**: plays on real hardware — the whole chain is ported
  (engine → strings → lo-fi tape → reverb, ambient brain, IMU gestures)
  with five visualization scenes and a menu-less UI. On top of that, all
  on the device now: the **heart** (tempo entrainment — a PLL on your key
  presses), the **soul** (the memory garden and pulse are saved at natural
  pauses; the garden keeps phrase boundaries and timing, while the box still
  greets you with one remembered note), a
  **WS2812 firefly** that flashes in the color of each note and dims with
  the battery, the **goodnight lullaby** (lay it
  face-down), **psychoacoustic bass** voiced for the 3 cm speaker, the
  grid tuned to the *physical* keyboard stagger (4-T-F-C is one chord),
  and the EDO scales lifted an octave into the same register as the JI
  scales.
- The shared garden, pulse, lullaby and background-preset models, both LCD
  languages, the scale registry and the hardware-independent face-down dwell
  have host tests. Both firmware languages are also built end to end; device
  integration still deserves a physical-device pass.

---

Priority: low latency and smooth sound — this is a live instrument.

## Layout

```
lib/grajek_audio/   synthesis engine — pure C++17, ZERO hardware dependencies
                    (sine-partial oscillators, ADSR, SVF filter,
                     scales: 12/19/31-EDO + 11-limit just intonation,
                     lock-free event queue; the same code runs on the ESP32
                     and on a laptop)
lib/grajek_core/    pure state models shared by device and host: phrase garden,
                    pulse entrainment, goodnight sequencer, background presets
host/               PC targets for shaping the sound without flashing
src/                ESP32 firmware (PlatformIO / Arduino core 3.x):
  hal/              I2S speaker output + ES8311, verified board pins
  input/            keyboard (TCA8418 polled over I2C) + BtnGO
  modes/            INSTRUMENT (play) and USTAWIENIA
  viz.cpp           five visualization scenes — same music, different worlds
  pulse.cpp         the heart: tempo entrainment (a PLL on key presses)
  soul.cpp          what survives the power switch (garden/chord/pulse, NVS)
  firefly.cpp       the WS2812 firefly + battery dusk
  i18n.cpp          compile-time LCD text (Polish default, English variant)
  settings.cpp      shared musical state (scale/timbre/octave/scene), in NVS
  main.cpp          main loop; boots straight into playing
```

## Playing on the laptop (no hardware needed)

```bash
cd host
make play        # grajek_live — REAL-TIME playing via CoreAudio (macOS)
make run         # grajek_host — renders demo WAVs into host/out/
make test        # shared state, registries, PL/EN text and face-down gesture
afplay out/01_single_note.wav
```

`grajek_live`: 4 laptop keyboard rows = 4 instrument grid rows (the bottom
`zxcv…` row plays lowest; a laptop row reaches 10–12 of the grid's 14
columns — enough to design with, the device has all 14). A terminal has no
key-up events, so a note fades 0.6 s after release (auto-repeat sustains
it), and **SHIFT+L toggles LATCH mode**: a key press turns a note on
permanently, a second press turns it off — that is how you build drones.
`TAB` scale, `` ` `` timbre, `BACKSPACE` octave, `SHIFT+B` A/Bs the
psychoacoustic bass, arrows = IMU stand-in (left/right bend, up/down
filter), `SHIFT+,` / `SHIFT+.` shake one remembered phrase down / up,
`ENTER` panic + re-center, `ESC` quit.

**Toss simulator** (a laptop-only toy — on the device the throw gave way to
the shake-rattle gesture): `SPACE` throws the whole music into a rising glissando
(synth bend + loop varispeed together), arrows **during flight** add spin
(right = land upward/otonal, left = land downward/utonal; more spin = flight
warble + a dizzy landing), `SPACE` again catches: flight time picks the rung
of the JI ladder (9/8, 5/4, 3/2, 7/4, 2/1) and everything lands re-rooted on
the new tonal center. `SHIFT+X` = fumble: a comic tape-dive stumble — the
music trips and gets back up, nothing is lost.

**Looper** (`lib/grajek_audio/src/ga_looper.*` — host-only for now, see the
roadmap): `SHIFT+R` starts the first recording, pressed again it closes
the loop and defines its length; from then on `SHIFT+R` toggles overdub
(each take commits one layer). `SHIFT+P` play/stop, `SHIFT+U` undo the
last layer, `SHIFT+C` clear, `SHIFT+[` / `SHIFT+]` loop playback volume
(default 80% — leaves headroom to play over the loop). `SHIFT+F` freezes
what you hear into a floor loop — and clears the echo tape only when the
looper actually accepts the capture. Record a phrase, let it circle,
layer on top — panic (`ENTER`) silences the synth but keeps the loop
running. Ghosts wait 7 s of silence, same as the device.

The engine applies equal-power polyphony compensation (1/sqrt(voices),
slowly smoothed), so a held chord sits at roughly the level of a single
note instead of squashing into the clipper.

## Flashing the Cardputer-ADV

PlatformIO lives in a project-local uv environment (`.venv`, gitignored):

```bash
uv venv .venv
uv pip install --python .venv/bin/python platformio==6.1.19 pip==26.2 pyyaml==6.0.3  # once
.venv/bin/pio run -e cardputer-adv -t upload     # Polish (default)
.venv/bin/pio run -e cardputer-adv-en -t upload  # English
.venv/bin/pio device monitor
```

`platformio.ini` pins the pioarduino platform to **55.03.311** so device
builds use one reproducible Arduino/ESP-IDF toolchain. The display language
is selected only while compiling: `cardputer-adv` is Polish and
`cardputer-adv-en` is English. It does not add a language setting or alter the
saved NVS format.

### Playing it: there is no menu

The box **boots straight into playing**. In INSTRUMENT all 56 keys are music:
column = scale step, bottom row = the lowest interval, and no playing key is
special. USTAWIENIA is the deliberate exception: digits 1–5 change its five
rows; any keyboard key can still wake the box.

Everything else hangs off the one side button:

| Gesture | What it does |
|---|---|
| any key while playing | plays (column = scale step, bottom row = lowest) |
| **short BtnGO** | toggles playing ⇄ settings screen |
| **hold BtnGO** | next timbre (repeats every 0.7 s, the name flashes) |
| **shake** | the *wind of memories* — each swing plays one short remembered phrase with its original rhythm; the waving direction steers an occasional whole-phrase transposition up or down |
| **tilt sideways** | brightness: darkens or opens the sound (filter) |
| **tilt toward / away** | depth: the sound moves into the reverb and echo, or comes close and dry |
| **lay face-down** (after playing) | goodnight: the screen and firefly switch off, then the day's phrases replay as a quiet, slowing lullaby — lifting the box, BtnGO or any keyboard key wakes it instantly |

The settings screen (short BtnGO) has five rows, each cycled by its digit
and persisted in NVS: **scale** (ordered happy → strange: pentatonic JI,
just major, 12-EDO, 19-EDO, 31-EDO, 11-limit Partch, WOLF), **timbre**,
**octave**, **background layer** (off → root → the breathing fifth drone → a
root/fifth/harmonic-seventh halo; shown in Polish as *cisza / fundament /
dron / aureola*), and
**visualization scene**.

Two design laws are enforced here rather than explained: the shake plays
*remembered phrases* instead of a scale, so the keyboard and the gesture
feed each other instead of competing; and both tilts are computed from a
low-passed gravity vector and smoothed over ~0.4 s, so shaking the box never
jerks the filter and a child's unsteady hands do not wobble the sound.

### The screen: five worlds, one music

The same notes, memories and gestures drive five scenes that differ in
*composition and motion*, not decoration:

- **laka** (meadow) — fireflies over grass; a note that ends drifts down
  and stays as a glowing seed. **This is the ghost garden made visible**:
  after a silence, remembered phrases wake their seeds in sequence.
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
loves the darkening anyway) → reverb → speaker voicing (a 180 Hz high-pass
plus a gentle presence shelf: the membrane is never asked for air it cannot
move, while the engine's **virtual-pitch bass** shifts each low note's
fundamental energy into partials that reconstruct the perceived bass) → a
hard output ceiling. The ambient brain (background chord, weather, ghost
garden) runs from boot. Still deferred: the FREEZE/looper floor (RAM audit
on real hardware first).

## Hardware — facts verified against official sources

Sources: M5Unified, M5GFX (ADV autodetect), M5Cardputer-UserDemo
(branch `CardputerADV`), schematic `Sch_M5CardputerAdv_v1.0`. Details and
quotes in `src/hal/board_pins.h`.

| Part | Configuration |
|---|---|
| ES8311 (speaker codec) | I2C `0x18` on G8/G9; I2S output: BCLK=G41, WS=G43, DOUT=G42; **no MCLK** — MCLK=BCLK mode |
| NS4150B amp | **no GPIO enable** — muted in hardware by an inserted 3.5 mm jack |
| Keyboard | **TCA8418** controller over I2C (`0x34`), INT=G11 — not a GPIO scan |
| IMU BMI270 | I2C `0x69` |
| LCD ST7789 | handled by M5GFX autodetect; backlight = **G38** |
| Power | mechanical slide switch — **there is no power-hold pin** (G38 is NOT power!) |
| IR / LED / SD | IR TX=G44, WS2812=G21, SD CS=G12 (SPI G40/G14/G39), battery ADC=G10 |
| EXT (LoRa cap) | CS=G5, RST=G3, BUSY=G6, DIO1=G4, SPI shared with SD; GPS UART: RX=G15, TX=G13 |

## Architecture decisions

- **Own I2S path instead of `M5.Speaker`** — skips the M5Unified mixer; the
  audio task runs on **core 0** (UI owns core 1), DMA buffers 4×96 frames,
  synthesis in 96-sample blocks @ 48 kHz. The ES8311 output setup follows
  the official M5Unified Cardputer-ADV playback callback.
- The engine never allocates after `init()` and is controlled exclusively
  through an SPSC queue — no mutexes anywhere near the audio path.
- **The keyboard is polled, not interrupt-driven.** M5Cardputer's TCA8418
  reader can wedge (INT low with the flag already cleared → the keyboard
  goes dead while the rest of the box keeps humming, which is exactly how
  it failed in a child's hands). We read the controller's event FIFO over
  I2C every pass instead, and do not call `M5Cardputer.update()` at all so
  nothing else drains that FIFO.
- Isomorphic grid: column = +1 scale step, row ≈ a fourth (EDO) or +octave
  (JI); the geometry follows from the scale (`ga_scales.cpp`). Two facts
  of physics are compensated in the mapping: the EDO grids are lifted one
  octave (fourth-stacked rows span far less than octave rows, so without
  the lift switching PENTA → EDO dropped the whole keyboard by over an
  octave), and the digit row maps to `col+1` because the physical top row
  is staggered one key to the right — the column the **eye** sees
  (4-T-F-C) is the column that plays one chord.
- Scales, visual scenes and background layers use small static descriptor
  registries. There is no dynamic loader or heap-backed plugin system on the
  instrument; adding an option remains modular while IDs and NVS ordering stay
  explicit and testable.
- Polish and English LCD strings are selected at compile time. A firmware
  image contains one language only, so the device carries no locale setting or
  duplicate runtime language state.

## What to check after flashing

1. It boots **playing** (a quiet drone) and every key sounds. If rows come
   out upside down, swap `3 - row` in `mode_instrument.cpp`.
2. Keys stay clean **with no clicks/dropouts** while the UI is stressed —
   hold several keys, shake, switch scenes.
3. Short BtnGO opens settings and returns; holding it while playing cycles
   the timbre; background cycles through silence, root, drone and halo; every
   choice survives a power cycle (NVS). Old boolean background settings migrate
   to silence or the original drone.
4. Shaking replays one remembered phrase per swing (with the captured
   rhythm, not an instant burst), and waving left/right steers the whole
   phrase down/up; both tilts respond smoothly
   — if an axis feels swapped, exchange `s_gravX` / `s_gravY` in
   `imuStep()`.
5. The 3.5 mm jack mutes the speaker (purely hardware — should just work).
   It is also the way to record the box cleanly: the tiny speaker does not
   do it justice.
6. Play a few even notes: a quiet MUSICBOX heartbeat joins in your tempo
   and lets go when you stop or drift into rubato.
7. After playing, leave five quiet seconds (or finish goodnight) so the
   natural-pause save completes; then power-cycle. A moment after boot it greets you
   with ONE remembered note — and ghosts never sow before new human play.
8. The firefly (WS2812) flashes with every note — the same note is always
   the same color — glows softly when a ghost replays a memory, and stays
   completely dark when idle.
9. Lay it face-down after playing: after 1.2 s the screen and firefly go dark
   while a gentler lullaby retells the day's phrases; lift it (or press BtnGO /
   any keyboard key, including in settings) and it wakes instantly.

## Roadmap

1. ✅ A playing core on the PC (WAV + live playing) and the ESP32 firmware
   with INSTRUMENT, gestures and the visualization scenes
2. ✅ The heart and the soul on the device: tempo entrainment, phrase-aware NVS memory
   with the waking greeting, the goodnight lullaby (the old "DRONE mode"
   idea, realized as a behavior instead of a mode — zero settings rows),
   the firefly, the psychoacoustic bass
3. LOOP — **deliberately parked**: a record/overdub
   transport is configuration (against design law 4), and RAM is on the
   edge. If it ever returns, it returns as a gesture, not a transport.
4. ~~IR CONDUCTOR~~ — dropped. It needs an IR receiver the ADV does not
   have, and it strengthens no design law; the IR TX pin stays free for an
   idea that earns its place.
