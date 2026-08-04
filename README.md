# grajek

A pocket instrument that **cannot be played badly** — built by a dad for his
kid, on an **M5Stack Cardputer-ADV** (ESP32-S3, ~$60 off the shelf).

https://github.com/user-attachments/assets/ff5505d3-2069-4a1e-8fab-654272a3c69c

You shake it, tilt it, click it — and something beautiful comes out.
Under the toy there is a serious synthesis engine: just
intonation in the spirit of Harry Partch, aging tape memory, sympathetic
string resonators, and a heartbeat that follows *your* tempo instead of
imposing one.

**Have a Cardputer-ADV? [Install Grajek in your browser](https://jasisz.github.io/grajek/)**
— English is the default image; Polish is one button beside it.

## Why

It started as an experimental microtonal instrument and, iteration by
iteration ("where's the fun?", "too dark", "I hate toys that nag"),
converged on its real purpose: a first instrument for a child. These are
the design laws that survived the process:

1. **Impossible to play badly.** Pentatonic just intonation by default,
   equal-loudness key tracking, polyphony compensation, no failure states —
   any random key, any random pair of keys, sounds intentional.
2. **Cat, not Furby.** The box never solicits play. Switch it on and it purrs,
   play and it remembers, lay it face-down and it goes quiet.
3. **Tempo is discovered, not declared.** There is no BPM anywhere. Play a
   few even notes and a quiet heart joins in *your* time (a phase-locked
   loop on your key presses); pause and it lets go.
4. **Discovery instead of configuration.** While playing, every grid key is
   music — there are no special keys among the playing keys. The default world
   (pentatonic + chimes) is safe for a toddler; deeper scales simply sit
   further down the settings list, ordered happy-to-strange, waiting for an
   older kid to find them. No age switch, no parent manual.
5. **Memory is the soul.** At natural pauses it saves what you taught it —
   your settings and short phrases with their timing — then greets you on the
   next boot with one remembered note.

The gestures on top: **shake** the box and it rattles the child's own
remembered phrases — a swing releases one short musical thought with the
captured timing kept inside gentle playback-safe bounds; waving direction
steers the occasional whole-phrase transposition up or down, and swing energy
sets the loudness; **tilt** it and everything darkens or brightens (slow and
smooth, like turning the box away from the light); **lay it face-down** after
playing and the remembered garden sings itself to sleep — and if it is simply
left alone for a few minutes, it reaches the same bedtime by itself.

## Status

- **Laptop rig (macOS)**: fully playable, this is where everything is
  designed and measured — see below.
- **Device firmware**: plays on real hardware — the whole chain is ported
  (engine → strings → chorus → lo-fi tape → reverb, ambient brain, IMU gestures)
  with five visualization scenes. It boots directly into play and has one
  settings screen behind BtnGO, rather than a mode menu. Also on the device:
  the **heart** (tempo entrainment — a PLL on your key
  presses), the **soul** (the memory garden and pulse are saved at natural
  pauses; the garden keeps phrase boundaries and timing, while the box still
  greets you with one remembered note), a
  **WS2812 firefly** that flashes in the color of foreground notes and dims
  with the battery, the **goodnight lullaby** (lay it face-down, or just stop
  playing for three minutes), **psychoacoustic bass** voiced for the 3 cm
  speaker, the
  grid tuned to the *physical* keyboard stagger (4-T-F-C is one chord),
  and the EDO scales lifted an octave into the same register as the JI
  scales.
- The synthesis presets and releases, stuck-note queue safety, chorus, echo,
  the inactivity timer that ends the day on its own,
  shared garden, pulse, lullaby and background-preset models, both LCD
  languages, scale registry and the hardware-independent face-down dwell and
  idle timer have host tests. The English and Polish firmware images are also verified with
  separate end-to-end PlatformIO builds.

---

Priority: low latency and smooth sound — this is a live instrument.

## Layout

```
lib/grajek_audio/   synthesis engine — pure C++17, ZERO hardware dependencies
                    (sine-partial + PolyBLEP saw/pulse oscillators, ADSR, SVF,
                     scales: pentatonic/major/11-limit JI, 12/19/31-EDO, WOLF,
                     lock-free event queue; the same code runs on the ESP32
                     and on a laptop)
lib/grajek_core/    pure state models shared by device and host: phrase garden,
                    pulse entrainment, goodnight sequencer, background presets
host/               PC targets for shaping the sound without flashing
web/                ESP Web Tools installer published with each tagged release
tools/              deterministic release/Pages packaging
.github/workflows/  host + firmware CI; tagged Releases and Pages deployment
src/                ESP32 firmware (PlatformIO / Arduino core 3.x):
  hal/              I2S speaker output + ES8311, verified board pins
  input/            keyboard (TCA8418 polled over I2C) + BtnGO
  modes/            INSTRUMENT (play) and USTAWIENIA
  viz.cpp           five visualization scenes — same music, different worlds
  ambient.cpp       device adapter for the garden, lullaby and background
  pulse.cpp         device/audio adapter for the shared pulse tracker
  soul.cpp          what survives the power switch (garden/chord/pulse, NVS)
  firefly.cpp       the WS2812 firefly + battery dusk
  i18n.cpp          compile-time LCD text (English default, Polish variant)
  settings.cpp      scale/timbre/octave/background/scene/glide state, in NVS
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

`grajek_live` requires macOS and the Xcode command-line tools. Its four laptop
keyboard rows mirror the four instrument grid rows (the bottom
`zxcv…` row plays lowest; a laptop row reaches 10–12 of the grid's 14
columns — enough to design with, the device has all 14). A terminal has no
key-up events, so a note fades 0.6 s after the last press or auto-repeat.

| Keys | Action |
|---|---|
| grid keys | play; `SHIFT+L` toggles latch mode for held drones/chords |
| `TAB` / `` ` `` / `BACKSPACE` | next scale / timbre / base octave |
| arrows | IMU stand-in: left/right pitch bend, up/down filter |
| `SHIFT+B` / `SHIFT+H` | psychoacoustic-bass A/B / harmonic shadow |
| `SHIFT+E` / `SHIFT+V` | echo on/off / reverb dry → subtle → default → cathedral |
| `SHIFT+T` / `SHIFT+S` | background on/off / sympathetic strings on/off |
| `SHIFT+D`, then grid keys | pick a custom background chord (toggle up to 4 notes); `SHIFT+D` accepts it |
| `SHIFT+,` / `SHIFT+.` | shake one remembered phrase down / up |
| `SHIFT+W` | start/stop the final-mix recorder; writes `session_<timestamp>.wav` |
| `ENTER` / `ESC` | panic + re-center (the loop keeps running) / quit |

The live rig restores its scale, timbre, octave, room, custom background and
phrase garden from `host/grajek_soul.txt` (created when you quit normally).

**Toss simulator** (a laptop-only toy — on the device the throw gave way to
the shake-rattle gesture): `SPACE` throws the whole music into a rising
glissando (synth bend + loop varispeed together), arrows **during flight** add spin
(right = land upward/otonal, left = land downward/utonal; more spin = flight
warble + a dizzy landing), `SPACE` again catches: flight time picks the rung
of the JI ladder (9/8, 5/4, 3/2, 7/4, 2/1) and everything lands re-rooted on
the new tonal center. `SHIFT+X` = fumble: a comic tape-dive stumble — the
music trips and gets back up, nothing is lost.

**Looper** (`lib/grajek_audio/src/ga_looper.*` — host-only): `SHIFT+R` starts
the first recording; pressed again, it closes the loop and defines its length.
From then on `SHIFT+R` toggles overdub
(each take commits one layer). `SHIFT+P` play/stop, `SHIFT+U` undo the
current overdub or last committed layer (8 levels), `SHIFT+C` clear,
`SHIFT+[` / `SHIFT+]` loop playback volume
(default 80% — leaves headroom to play over the loop). `SHIFT+F` freezes
what you hear into a floor loop — and clears the echo tape only when the
looper actually accepts the capture. Record a phrase, let it circle,
layer on top — panic (`ENTER`) silences the synth but keeps the loop
running. Ghosts become eligible after 7 s of silence, then return after a
randomized pause, same as the device.

The engine applies equal-power polyphony compensation (1/sqrt(voices),
slowly smoothed), so a held chord sits at roughly the level of a single
note instead of squashing into the clipper.

## Installing on the Cardputer-ADV

The published images are for the **M5Stack Cardputer-ADV only**, not the
original Cardputer.

### Browser — easiest

Open the **[Grajek web installer](https://jasisz.github.io/grajek/)** in a
desktop browser with Web Serial support, connect a data-capable USB-C cable and
choose English or Polish. No local toolchain is needed. Close any serial
monitor first; when the browser asks for a port, choose the USB JTAG/serial
device. If no port appears, switch the box off, hold G0 while reconnecting or
switching it on, release G0 and try again.

The browser writes a complete image from flash offset `0x0`, so it replaces the
current firmware and clears saved Grajek settings and remembered phrases.
Tagged versions and SHA-256 checksums are also available under
**[GitHub Releases](https://github.com/jasisz/grajek/releases)**.

### M5Launcher

M5Launcher 2.8 or newer supports the Cardputer-ADV. Download
`grajek-cardputer-adv.bin` (English) or `grajek-cardputer-adv-pl.bin` (Polish)
from the latest release and install the same unzipped file from an OTA Favorite,
the Launcher WebUI or an SD card. The files are merged images, which Launcher
recognizes and safely installs into its own app layout.

### Build and flash from source

PlatformIO lives in a project-local uv environment (`.venv`, gitignored).
You need `uv` and a data-capable USB cable:

```bash
uv venv .venv
uv pip install --python .venv/bin/python platformio==6.1.19 pip==26.2 pyyaml==6.0.3  # once
.venv/bin/pio run -e cardputer-adv               # build English (default)
.venv/bin/pio run -e cardputer-adv-pl            # build Polish
.venv/bin/pio run -e cardputer-adv -t upload     # upload English
.venv/bin/pio run -e cardputer-adv-pl -t upload  # OR upload Polish
.venv/bin/pio device monitor
```

`platformio.ini` pins the pioarduino platform to **55.03.311** so device
builds use one reproducible Arduino/ESP-IDF toolchain. The display language
is selected only while compiling: `cardputer-adv` is English and
`cardputer-adv-pl` is Polish. It does not add a language setting or alter the
saved NVS format.

### Playing it: straight to the instrument

The box **boots straight into playing**: there is no mode picker or setup
wizard. In INSTRUMENT all 56 keys are music: column = scale step, bottom row =
the lowest interval, and the digit row follows the keyboard's physical
one-column stagger. USTAWIENIA is the deliberate exception: digits 1–6 change
its six rows; every other keyboard key only wakes the box.

Everything else hangs off the one side button:

| Gesture | What it does |
|---|---|
| any key while playing | plays (column = scale step, bottom row = lowest) |
| **short BtnGO** | toggles playing ⇄ settings screen |
| **hold BtnGO while playing** | next timbre (repeats every 0.7 s, the name flashes) |
| **shake** | the *wind of memories* — when no replay is active, a swing starts one short remembered phrase with bounded captured timing; an empty garden answers with one safe note, and waving direction steers an occasional whole-phrase transposition up or down |
| **tilt sideways** | brightness: darkens or opens the sound (filter) |
| **tilt toward / away** | depth: the sound moves into the reverb and echo, or comes close and dry |
| **lay face-down** (after playing) | goodnight: the screen and firefly switch off, then recent remembered phrases replay as a quiet, slowing lullaby — lifting the box, BtnGO or any keyboard key wakes it instantly |

The settings screen (short BtnGO) has six rows, each cycled by its digit
and persisted in NVS: **scale** (ordered happy → strange: pentatonic JI,
just major, 12-EDO, 19-EDO, 31-EDO, 11-limit Partch, WOLF), **timbre**
(the five additive colors plus WARM filtered saw and HOLLOW breathing pulse),
**octave**, **background layer** (off → root → a breathing two-note drone whose
upper voice moves between fifth and harmonic seventh → a fixed
root/fifth/harmonic-seventh halo; the Polish labels are *cisza*, *fundament*,
*dron* and *aureola*), **visualization scene**, and **glide** (off → soft →
strong). Glide is off by default. SOFT gives quick neighbouring notes in one
physical row a short pitch landing; STRONG waits longer between keys, forgives
much wider leaps and sings the whole way, a deliberate portamento. Simultaneous
chords and the preceding voice remain polyphonic in both.

Two design laws are enforced here rather than explained: the shake plays
*remembered phrases* instead of a scale, so the keyboard and the gesture
feed each other instead of competing; and both tilts are computed from a
low-passed gravity vector and smoothed over ~0.4 s, so shaking the box never
jerks the filter and a child's unsteady hands do not wobble the sound.

### The screen: five worlds, one music

The same notes, memories and gestures drive five scenes that differ in
*composition and motion*, not decoration:

- **laka / meadow** — fireflies over grass; a note that ends drifts down
  and stays as a glowing seed. **This is the ghost garden made visible**:
  after a silence, remembered phrases wake their seeds in sequence.
- **kosmos / cosmos** — radial: notes **orbit** a pulsing core (higher = further
  and faster), memories are stars in an outer ring; sideways tilt spins the
  whole system, depth tilt flattens the orbital plane.
- **ocean** — flow: fish **swim across** the screen and wrap around, light
  shafts lean with the tilt, memories sink to the sand as pearls, the
  shake sends bubbles up.
- **ognie / fireworks** — expansion: each note **blooms as a growing ring**
  and falls back as embers; near-empty sky, all attention on the notes.
- **mandala** — abstraction: sounding notes form a symmetric rotating
  pattern; depth tilt changes the symmetry from 4 to 9 arms.

### Sound chain on the device

Same portable modules as the host: engine → sympathetic strings → a subtle
preset-specific chorus → echo tape (3 s at 16 kHz behind a lo-fi rate bridge —
no PSRAM, and the aging tape loves the darkening anyway) → reverb → speaker
voicing (a 180 Hz high-pass plus a gentle presence shelf: the membrane is never
asked for air it cannot move, while the engine's **virtual-pitch bass** shifts each low note's
fundamental energy into partials that reconstruct the perceived bass when the
timbre's partial structure permits it) → a hard output ceiling. The main tape
head remains a free three-second memory; its quieter second head gradually
answers at a dotted eighth of the pulse only while the player's tempo PLL is
confident, then drifts back to its free golden-ratio position. The ambient
brain (background chord, weather, ghost
garden) runs from boot.

## Hardware — facts verified against official sources

Sources: M5Unified, M5GFX (ADV autodetect), M5Cardputer-UserDemo
(branch `CardputerADV`), schematic `Sch_M5CardputerAdv_v1.0`. Details and
source notes are in `src/hal/board_pins.h`.

| Part | Configuration |
|---|---|
| ES8311 (speaker codec) | I2C `0x18` on G8/G9; I2S output: BCLK=G41, WS=G43, DOUT=G42; **no MCLK** — MCLK=BCLK mode |
| NS4150B amp | **no GPIO enable** — muted in hardware by an inserted 3.5 mm jack |
| Keyboard | **TCA8418** controller over I2C (`0x34`), INT=G11 — not a GPIO scan |
| IMU BMI270 | I2C `0x69` |
| LCD ST7789 | handled by M5GFX autodetect; backlight = **G38** |
| Power | mechanical slide switch — **there is no power-hold pin** (G38 is NOT power!) |
| IR / LED / SD | IR TX=G44, WS2812=G21, SD CS=G12 (SPI G40/G14/G39), battery ADC=G10 |
| EXT header | CS=G5, RST=G3, BUSY=G6, DIO1=G4, SPI shared with SD; UART: RX=G15, TX=G13 |

## Architecture decisions

- **Own I2S path instead of `M5.Speaker`** — skips the M5Unified mixer; the
  audio task runs on **core 0** (UI owns core 1), DMA buffers 4×96 frames,
  synthesis in 96-sample blocks @ 48 kHz. The rate is a CPU budget as much as a
  bandwidth choice: one block must be finished inside 2 ms or the audio task
  stops yielding and starves the idle task into a watchdog reset. It fits at
  ~69%; the ambience (strings, reverb, tape) runs behind lo-fi bridges at a
  whole-number fraction of it, so only the wet path is band-limited while the
  played notes stay full rate. The ES8311 output setup follows
  the official M5Unified Cardputer-ADV playback callback.
- The engine never allocates after `init()` and is controlled exclusively
  through an SPSC queue — no mutexes anywhere near the audio path.
- **A fixed compile-time audio path, not ESP-GMF.** This instrument has
  one small real-time chain and its own low-latency I2S endpoint; a graph
  framework would add lifecycle and integration surface without
  improving the current signal flow.
- **The keyboard is polled, not interrupt-driven.** M5Cardputer's TCA8418
  reader can wedge (INT low with the flag already cleared → the keyboard
  goes dead while the rest of the box keeps humming, which is exactly how
  it failed in a child's hands). We read the controller's event FIFO over
  I2C every pass instead, and do not call `M5Cardputer.update()` at all so
  nothing else drains that FIFO.
- Isomorphic grid: column = +1 scale step; rows move by two scale steps in
  PENTA/MAJOR JI, roughly a fourth in EDO, one octave in 11-limit JI, and
  637 cents in WOLF. The geometry follows from the scale (`ga_scales.cpp`).
  Two facts of physics are compensated in the mapping: the EDO grids are
  lifted one octave (fourth-stacked rows span far less than octave rows, so without
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

1. With fresh NVS it boots **playing** with PENTA JI, CHIME, 220 Hz, the quiet
   drone and meadow. Otherwise it restores the saved background and other
   settings. Every key sounds; if rows come out upside down, swap `3 - row`
   in `mode_instrument.cpp`.
2. Keys stay clean **with no clicks/dropouts** while the UI is stressed —
   hold several keys, shake, switch scenes.
3. Short BtnGO opens settings and returns; holding it while playing cycles
   all seven timbres; background cycles through silence, root, drone and halo;
   glide starts off and, in row 6, cycles off → soft → strong, affecting quick
   same-row runs without collapsing chords; every choice survives a power
   cycle (NVS), and the old boolean glide setting migrates to off/soft.
   Old boolean background settings migrate to silence or the original drone.
4. When no replay is active, shaking starts one remembered phrase (with its
   timing bounded to 70–1200 ms between notes, not an instant burst); an empty
   garden produces one safe note. Waving left/right steers the whole phrase
   down/up; both tilts respond smoothly
   — if an axis feels swapped, exchange `s_gravX` / `s_gravY` in
   `imuStep()`.
5. The 3.5 mm jack mutes the speaker (purely hardware — should just work).
   It is also the way to record the box cleanly: the tiny speaker does not
   do it justice.
6. Play a few even notes: a quiet MUSICBOX heartbeat joins in your tempo
   and lets go after you stop.
7. After playing, leave five quiet seconds so the natural-pause save completes.
   Alternatively, let goodnight reach silence and leave it there for another
   three seconds. Then power-cycle: a moment after boot it greets you with ONE
   remembered note. Apart from that greeting, autonomous phrase ghosts wait
   for new human play.
8. The firefly (WS2812) flashes with foreground notes — the same note is always
   the same color — glows softly when a ghost replays a memory, and stays
   completely dark when idle.
9. Lay it face-down after playing: after the 1.2 s dwell the screen and firefly
   go dark; the first note of a gentler lullaby follows about 1.2 s later and
   retells recent remembered phrases. Lift it (or press BtnGO / any keyboard
   key, including in settings) and it wakes instantly.

## Next: ENSEMBLE

The next experiment is a small flock of Grajki that notice one another without
a phone, router or pairing screen. Each box would periodically broadcast a
tiny musical presence — enough for nearby instruments to discover the flock,
share a pulse or tonal context, and occasionally answer, never raw audio.

The transport is deliberately undecided until it is prototyped: **BLE
advertisements**, **ESP-NOW connectionless broadcast packets**, or both. No
session has to be established before a box can announce itself and throw a
small packet into the room; the prototype will decide which path makes group
play feel most automatic. It is a little unhinged, which is exactly why it
belongs here.
