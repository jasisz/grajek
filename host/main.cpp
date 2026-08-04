// PC target: the same engine that runs on the ESP32, rendered to WAV files.
// Usage: ./grajek_host [output_dir]
#include <stdio.h>
#include <math.h>
#include <algorithm>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "ga_echo.h"
#include "ga_chorus.h"
#include "ga_engine.h"
#include "ga_reverb.h"
#include "ga_scales.h"
#include "ga_strings.h"
#include "wav_writer.h"

using namespace ga;

namespace {

constexpr int kSr = 48000;
constexpr int kBlock = 128;
bool g_renderOk = true;

struct Cue {
  double t;
  std::function<void(Engine&)> fn;
};

// Renders through the FULL instrument chain (strings, aging tape, reverb) —
// the demo files should sound like the instrument, not like a bare engine.
void renderSong(const std::string& path, double seconds, Engine& e,
                std::vector<Cue> cues, float rootHz = 220.0f,
                float chorusDepth = 0.0f) {
  std::stable_sort(cues.begin(), cues.end(),
                   [](const Cue& a, const Cue& b) { return a.t < b.t; });

  static Reverb reverb;  // ~55 KB of state — keep it off the stack
  reverb.init(kSr);
  reverb.setWet(0.35f);
  SympatheticStrings strings;
  strings.init(kSr);
  strings.setRootHz(rootHz);
  Chorus chorus;
  chorus.init(kSr);
  chorus.setDepth(chorusDepth);
  std::vector<int16_t> tape((size_t)(4.0 * kSr));
  EchoTape echo;
  echo.init(tape.data(), (uint32_t)tape.size(), (float)kSr);
  echo.setFeedback(0.55f);
  echo.setLevel(0.5f);

  const size_t total = (size_t)(seconds * kSr);
  std::vector<int16_t> pcm;
  pcm.reserve(total);
  float buf[kBlock];
  size_t done = 0, cueIdx = 0;
  float peak = 0.0f;
  double sumSq = 0.0;
  while (done < total) {
    const double t = (double)done / kSr;
    while (cueIdx < cues.size() && cues[cueIdx].t <= t) {
      cues[cueIdx].fn(e);
      ++cueIdx;
    }
    const int n = (int)std::min((size_t)kBlock, total - done);
    e.process(buf, n);
    strings.process(buf, buf, n);
    chorus.process(buf, n);
    echo.process(buf, buf, n);
    reverb.process(buf, n);
    for (int i = 0; i < n; ++i) buf[i] = softClip(buf[i]) * 0.85f;
    for (int i = 0; i < n; ++i) {
      const float s = buf[i];
      peak = std::max(peak, fabsf(s));
      sumSq += (double)s * s;
      float c = s > 1.0f ? 1.0f : (s < -1.0f ? -1.0f : s);
      pcm.push_back((int16_t)(c * 32767.0f));
    }
    done += n;
  }
  if (!writeWavMono16(path.c_str(), pcm.data(), pcm.size(), kSr)) {
    fprintf(stderr, "ERROR: cannot write %s\n", path.c_str());
    g_renderOk = false;
    return;
  }
  const double rms = sqrt(sumSq / (double)total);
  printf("  %-38s %5.1fs  peak %.3f  rms %6.1f dBFS\n", path.c_str(), seconds,
         peak, 20.0 * log10(rms + 1e-12));
}

void demoSingleNote(const std::string& dir) {
  Engine e;
  e.init(kSr);
  std::vector<Cue> c = {
      {0.00, [](Engine& en) {
         en.setParam(Param::TimbrePreset, 0);
         en.setParam(Param::BaseHz, 220.0f);
         en.setParam(Param::FilterCutoffHz, 6000.0f);
       }},
      {0.05, [](Engine& en) { en.noteOn(1, 0.0f, 0.9f); }},
      {2.05, [](Engine& en) { en.noteOff(1); }},
  };
  renderSong(dir + "/01_single_note.wav", 3.5, e, c);
}

void demoGrid(const std::string& dir) {
  Engine e;
  e.init(kSr);
  std::vector<Cue> c = {
      {0.00, [](Engine& en) {
         en.setParam(Param::TimbrePreset, 0);
         en.setParam(Param::BaseHz, 220.0f);
         en.setParam(Param::FilterCutoffHz, 6000.0f);
       }},
  };
  // walk the bottom grid row in 19-EDO...
  double t = 0.1;
  for (int col = 0; col < kGridCols; ++col) {
    const float cents = gridToCents(ScaleId::EDO19, col, 0);
    c.push_back({t, [cents, col](Engine& en) { en.noteOn(100 + col, cents, 0.85f); }});
    c.push_back({t + 0.26, [col](Engine& en) { en.noteOff(100 + col); }});
    t += 0.28;
  }
  // ...then the same walk in just intonation (11-limit) for comparison
  t += 0.6;
  for (int col = 0; col < kGridCols; ++col) {
    const float cents = gridToCents(ScaleId::JI11, col, 0);
    c.push_back({t, [cents, col](Engine& en) { en.noteOn(200 + col, cents, 0.85f); }});
    c.push_back({t + 0.26, [col](Engine& en) { en.noteOff(200 + col); }});
    t += 0.28;
  }
  renderSong(dir + "/02_grid_19edo_and_ji.wav", t + 1.5, e, c);
}

void demoDrone(const std::string& dir) {
  Engine e;
  e.init(kSr);
  std::vector<Cue> c = {
      {0.00, [](Engine& en) {
         en.setParam(Param::TimbrePreset, 1);
         en.setParam(Param::BaseHz, 110.0f);
         en.setParam(Param::FilterCutoffHz, 700.0f);
         en.setParam(Param::FilterRes, 0.25f);
         en.setParam(Param::MasterGain, 0.45f);
       }},
      // JI chord: 1/1, 3/2, 7/4, 5/4 an octave up
      {0.20, [](Engine& en) { en.noteOn(1, 0.0f, 1.0f); }},
      {2.00, [](Engine& en) { en.noteOn(2, 702.0f, 0.8f); }},
      {4.00, [](Engine& en) { en.noteOn(3, 968.8f, 0.7f); }},
      {6.00, [](Engine& en) { en.noteOn(4, 1586.3f, 0.55f); }},
      {17.0, [](Engine& en) { en.allNotesOff(); }},
  };
  // slow filter open/close — breathing
  for (double tt = 0.5; tt < 17.0; tt += 0.25) {
    const float cut = 650.0f + 500.0f * (0.5f + 0.5f * (float)sin(kTau * tt / 13.0));
    c.push_back({tt, [cut](Engine& en) { en.setParam(Param::FilterCutoffHz, cut); }});
  }
  renderSong(dir + "/03_drone.wav", 20.0, e, c, 110.0f);
}

void demoBroadColours(const std::string& dir) {
  const float phrase[] = {0.0f, 386.3f, 702.0f, 1200.0f, 702.0f, 386.3f};
  for (int preset = 5; preset <= 6; ++preset) {
    Engine e;
    e.init(kSr);
    std::vector<Cue> cues = {
        {0.00, [preset](Engine& en) {
           en.setParam(Param::TimbrePreset, (float)preset);
           en.setParam(Param::BaseHz, 220.0f);
           en.setParam(Param::FilterCutoffHz, 7500.0f);
         }},
    };
    double t = 0.12;
    for (int i = 0; i < 6; ++i) {
      const float cents = phrase[i];
      const float fromCents = i > 0 ? phrase[i - 1] : cents;
      cues.push_back({t, [i, cents, fromCents](Engine& en) {
                        if (i > 0)
                          en.noteOnGlide(20 + i, fromCents, cents, 0.82f);
                        else
                          en.noteOn(20, cents, 0.82f);
                      }});
      cues.push_back({t + 0.34,
                      [i](Engine& en) { en.noteOff(20 + i); }});
      t += 0.38;
    }
    const Timbre timbre = timbrePreset(preset);
    const char* name = preset == 5 ? "04_warm.wav" : "05_hollow.wav";
    renderSong(dir + "/" + name, t + 2.0, e, cues, 220.0f,
               timbre.chorusDepth);
  }
}

void printGridTables() {
  printf("\nGrid (bottom row, BaseHz=220):\n");
  for (int s = 0; s < (int)ScaleId::Count; ++s) {
    const ScaleId id = (ScaleId)s;
    printf("  %-12s (%s)\n    ", scaleInfo(id).name, scaleInfo(id).desc);
    for (int col = 0; col < kGridCols; ++col) {
      const float hz = 220.0f * exp2f(gridToCents(id, col, 0) / 1200.0f);
      printf("%.1f ", hz);
    }
    printf("Hz\n");
  }
}

}  // namespace

int main(int argc, char** argv) {
  const std::string dir = argc > 1 ? argv[1] : "out";
  std::error_code dirError;
  std::filesystem::create_directories(dir, dirError);
  if (dirError || !std::filesystem::is_directory(dir)) {
    fprintf(stderr, "ERROR: cannot create output directory %s\n", dir.c_str());
    return 1;
  }
  printf("grajek_host — engine at %d Hz, %d-sample blocks\n", kSr, kBlock);
  printf("Rendering into: %s/\n\n", dir.c_str());
  demoSingleNote(dir);
  demoGrid(dir);
  demoDrone(dir);
  demoBroadColours(dir);
  printGridTables();
  printf("\nListen:  afplay %s/01_single_note.wav\n", dir.c_str());
  return g_renderOk ? 0 : 1;
}
