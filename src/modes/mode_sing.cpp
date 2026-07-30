#include "mode_sing.h"

#include <Arduino.h>

#include "../duet.h"
#include "../hal/audio_out.h"
#include "../settings.h"
#include "../viz.h"

namespace {
// companion voice id — outside the grid (0..55), background (1000+),
// ghosts (1100+) and chimes (900+)
constexpr int32_t kDuetId = 1400;
// window policy: open after this much key silence with a calm synth env,
// close after this much mic silence once a voice was heard
constexpr uint32_t kOpenAfterMs = 400;
constexpr uint32_t kCloseAfterMs = 350;
constexpr float kEnvQuiet = 0.05f;
}  // namespace

void ModeSing::enter(ModeCtx& ctx) {
  ModeInstrument::enter(ctx);
  duet::reset();
  hal::earSetOpen(true);  // arm; windows open when the keys go quiet
  st_ = St::Idle;
  lastKeyMs_ = millis();
  cooldownUntil_ = lastKeyMs_ + 600;
  heardVoice_ = false;
  // bez ucha (brak mikrofonu w konfiguracji) tryb dalej gra — tylko nie słyszy
  viz::toast(hal::earAvailable() ? "spiewaj!" : "ucho spi :(");
}

void ModeSing::exit(ModeCtx& ctx) {
  if (st_ == St::Answer) ctx.engine.noteOff(kDuetId);
  hal::earSetOpen(false);  // aborts a live window too
  viz::setListening(false);
  viz::setVoiceLevel(0.0f);
  ModeInstrument::exit(ctx);
}

void ModeSing::onKey(ModeCtx& ctx, int col, int row, bool down) {
  // KLAWISZE ZAWSZE WYGRYWAJĄ: pierwszy klawisz w oknie oddaje magistralę
  // głośnikowi, zanim nuta wejdzie w silnik — chwila ciszy maskuje rytuał
  if (down) {
    lastKeyMs_ = millis();
    if (st_ == St::Listen) {
      hal::earWindowClose();
      viz::setListening(false);
      viz::setVoiceLevel(0.0f);
      st_ = St::Idle;
      cooldownUntil_ = lastKeyMs_ + 800;
    }
  }
  ModeInstrument::onKey(ctx, col, row, down);
}

void ModeSing::tick(ModeCtx& ctx, float dt) {
  ModeInstrument::tick(ctx, dt);
  const uint32_t now = millis();

  switch (st_) {
    case St::Idle:
      // klawisze ucichły, synth wybrzmiał — pudełko nadstawia ucho
      if (hal::earAvailable() && now > cooldownUntil_ &&
          now - lastKeyMs_ > kOpenAfterMs && hal::audioEnv() < kEnvQuiet) {
        if (hal::earWindowOpen()) {
          duet::reset();
          st_ = St::Listen;
          heardVoice_ = false;
          viz::setListening(true);
        } else {
          cooldownUntil_ = now + 1500;
        }
      }
      break;

    case St::Listen:
      duet::tick();
      viz::setVoiceLevel(hal::earLevel() * 2.5f);  // pierścień słyszy głos
      if (hal::earVoicePresent()) {
        lastVoiceMs_ = now;
        heardVoice_ = true;
      }
      // śpiew był i ucichł: oddaj głośnik i odpowiedz
      if (heardVoice_ && now - lastVoiceMs_ > kCloseAfterMs) {
        hal::earWindowClose();
        viz::setListening(false);
        viz::setVoiceLevel(0.0f);
        if (hal::earAnswerStart()) {
          st_ = St::Answer;
          answerStartMs_ = millis();
          trackIdx_ = 0;
          if (duet::trackCount() > 0) viz::toast("duet!");
        } else {
          st_ = St::Idle;
          cooldownUntil_ = now + 800;
        }
      }
      break;

    case St::Answer: {
      // towarzysz nuci pod odpowiedzią: nagrana ścieżka duetu odtwarzana
      // w rytmie głosu (offsety w próbkach mikrofonu, 16 na milisekundę)
      const uint32_t played =
          (millis() - answerStartMs_) * (uint32_t)(hal::earSampleRate() / 1000.0f);
      while (trackIdx_ < duet::trackCount() &&
             duet::trackOffset(trackIdx_) <= played) {
        const float comp = duet::trackCents(trackIdx_++);
        // legato: to samo id + glide + miękki PURE przez kanapkę FIFO
        ctx.engine.setParam(ga::Param::TimbrePreset, 0.0f);
        ctx.engine.setParam(ga::Param::GlideSec, 0.08f);
        ctx.engine.noteOn(kDuetId, comp, 0.4f);
        ctx.engine.setParam(ga::Param::GlideSec, 0.0f);
        ctx.engine.setParam(ga::Param::TimbrePreset,
                            (float)settings::preset());
      }
      if (!hal::earAnswerActive()) {
        ctx.engine.noteOff(kDuetId);
        st_ = St::Idle;
        cooldownUntil_ = millis() + 800;  // oddech przed kolejnym oknem
      }
      break;
    }
  }
}
