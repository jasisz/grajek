// Tempo entrainment state machine shared by firmware and the laptop rig.
#pragma once

namespace gk {

struct PulseBeat {
  bool play = false;
  int chordIndex = -1;
  float velocity = 0.0f;
};

class PulseTracker {
 public:
  void onOnset(double nowSec);
  PulseBeat tick(double nowSec, int chordNoteCount);

  // Stop the live heart while retaining its remembered tempo/grid.
  // Returns true when a sounding tracker was stopped.
  bool suspend();

  bool ticking() const { return ticking_; }
  double periodSec() const { return period_; }
  double memoryPeriodSec() const { return memory_; }
  double lastOnsetSec() const { return lastOnset_; }
  void restoreMemory(double periodSec) { memory_ = periodSec; }

 private:
  double onsets_[8]{};
  int count_ = 0;
  double period_ = 0.0;
  double nextBeatAt_ = 0.0;
  double lastOnset_ = 0.0;
  int beatsAlone_ = 0;
  int beatsPlayed_ = 0;
  int step_ = 0;
  bool ticking_ = false;
  double memory_ = 0.0;
};

}  // namespace gk
