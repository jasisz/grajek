// Temporary mode placeholder — holds a menu slot and shows the plan.
#pragma once
#include "mode.h"

class ModeStub : public Mode {
 public:
  ModeStub(const char* nm, const char* desc) : name_(nm), desc_(desc) {}
  const char* name() const override { return name_; }
  void draw(ModeCtx& ctx) override {
    auto& g = ctx.canvas;
    g.fillSprite(TFT_BLACK);
    g.setTextSize(2);
    g.setTextColor(TFT_WHITE, TFT_BLACK);
    g.drawString(name_, 8, 20);
    g.setTextSize(1);
    g.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    g.drawString("(under construction)", 8, 45);
    g.drawString(desc_, 8, 70);
    g.setTextColor(TFT_WHITE, TFT_BLACK);
    g.drawString("GO = back to menu", 8, 122);
  }

 private:
  const char* name_;
  const char* desc_;
};
