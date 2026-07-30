// Mode interface. A mode gets the shared core (engine + canvas) and changes
// only the input mapping and the sound character — adding a new mode is one
// class plus a branch in main.cpp (a short GO press toggles playing <->
// settings; there is no menu and no modes[] table).
#pragma once
#include <M5GFX.h>

#include "ga_engine.h"

struct ModeCtx {
  ga::Engine& engine;
  M5Canvas& canvas;
};

class Mode {
 public:
  virtual ~Mode() = default;
  virtual const char* name() const = 0;
  virtual void enter(ModeCtx&) { markDirty(); }
  virtual void exit(ModeCtx&) {}
  // col 0..13, row 0..3 (row 0 = the top physical keyboard row)
  virtual void onKey(ModeCtx&, int col, int row, bool down) {
    (void)col; (void)row; (void)down;
  }
  // held GO (a short GO press toggles playing <-> settings in main.cpp);
  // repeats every ~0.7 s while the button stays down
  virtual void onGoHold(ModeCtx&) {}
  virtual void tick(ModeCtx&, float dt) { (void)dt; }
  virtual void draw(ModeCtx&) = 0;

  bool dirty() const { return dirty_; }
  void clearDirty() { dirty_ = false; }

 protected:
  void markDirty() { dirty_ = true; }

 private:
  bool dirty_ = true;
};
