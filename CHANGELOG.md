# Changelog

## [0.2.0] — 2026-08-04

### Added
- Two timbres: WARM (detuned saw through a key-tracked lowpass) and HOLLOW
  (breathing pulse). Seven in total.
- Glide, three states: off / soft / strong (settings row 6).
- Output mode: speaker / jack (settings row 7). Jack drops the virtual bass,
  the 180 Hz high-pass and the presence lift — all of it exists for the 3 cm
  driver and hurts a real speaker.
- Falls asleep on its own after three minutes without a player, exactly as if
  laid face down. The ambient layer does not count as activity.
- Subtle per-preset chorus; the tape's second head answers a steady pulse at a
  dotted eighth.

### Changed
- Ambience (strings, reverb, tape) runs behind lo-fi rate bridges — only the wet
  path is band-limited, played notes stay full rate.
- Polyphony 10 → 8 voices. It is a CPU budget, not a taste.
- Reverb delay lengths and the tape rate now derive from the sample rate instead
  of being hand-kept constants that could disagree with reality.
- DRONE timbre → ORGAN (PL: ORGANY); it collided with the *drone* background
  layer, and a stack of octaves with a slow attack is an organ registration.
  PL: CIEPLA → AKSAMIT, PUSTA → FUJARKA.

### Fixed
- **The box rebooted during play.** Two or three notes were enough: the audio
  task overran its block, starved the idle task and tripped the watchdog.
  3045 µs → 1385 µs per block at 48 kHz (152% → 69% of budget).
- The audio library was compiling at `-Os` — the framework appends it after our
  `-O2`. Worth ~30% of the engine on its own.
- Two `double` literals on the audio path; this chip emulates double in software.
- HOLLOW rasped up the keyboard (a notch cannot tame a pulse) and WARM vanished
  up there, 13 dB down. Both now key-track their filter.
- Loudness spread between timbres: 10.7 dB → 6.0 dB. Not closed further on
  purpose: matching them exactly drove the engine bus into hard clipping.
- Voice stealing prefers the quietest releasing voice, not merely the oldest.
- Switching output mode ticked: both voicings shared one filter state, so the
  jack's identity shelf dumped the speaker shelf's history in two samples. Each
  voicing now keeps its own state and the handover crossfades.
- The background drone no longer snaps pitch mid-attack at boot.

## [0.1.2]
Public firmware releases and the web installer.

## [0.1.1]
Isolated body-resonance probe.

## [0.1.0]
First tagged instrument: five timbres, scales, sympathetic strings, echo tape,
reverb, ambient brain, IMU gestures, goodnight lullaby, five scenes.
