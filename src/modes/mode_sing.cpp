#include "mode_sing.h"

#include <Arduino.h>

#include "../ambient.h"
#include "../duet.h"
#include "../firefly.h"
#include "../hal/audio_out.h"
#include "../settings.h"
#include "../viz.h"

namespace {
// companion voice id — outside the grid (0..55), background (1000+),
// ghosts (1100+) and chimes (900+)
constexpr int32_t kDuetId = 1400;
// window policy: open after this much key/chime silence with no key held,
// close after this much mic silence once a voice was heard.
// NOTE: no audioEnv() gate here — the tampura hums the envelope up
// constantly, so a "quiet synth" condition never came true on hardware
// and the ear never opened. The release tails get a short fade instead
// (see the park fade in audio_out.cpp).
constexpr uint32_t kOpenAfterMs = 800;
constexpr uint32_t kCloseAfterMs = 350;
// a voiceless window falls asleep after this long: without it the mic ran
// all night, the box played dead until a key, and any room voice made it
// answer on its own — three Furby crimes in one absorbing state
constexpr uint32_t kListenTimeoutMs = 90000;
}  // namespace

void ModeSing::enter(ModeCtx& ctx) {
  ModeInstrument::enter(ctx);
  duet::reset();
  hal::earSetOpen(true);  // arm; windows open when the keys go quiet
  st_ = St::Idle;
  dormant_ = false;
  lastKeyMs_ = millis();
  // a longer first breath: the soul's waking-up note (boot + ~2.5 s) must
  // sound through a LIVE speaker, not into a parked one
  cooldownUntil_ = lastKeyMs_ + 4000;
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
    ++heldCount_;
    dormant_ = false;  // a key wakes the sleeping ear
    if (st_ == St::Listen) {
      hal::earWindowClose();
      viz::setListening(false);
      viz::setVoiceLevel(0.0f);
      st_ = St::Idle;
      cooldownUntil_ = lastKeyMs_ + 800;
    }
  } else {
    lastKeyMs_ = millis();  // release also postpones the window
    if (heldCount_ > 0) --heldCount_;
  }
  ModeInstrument::onKey(ctx, col, row, down);
}

void ModeSing::tick(ModeCtx& ctx, float dt) {
  ModeInstrument::tick(ctx, dt);
  const uint32_t now = millis();

  switch (st_) {
    case St::Idle:
      // klawisze i dzwonki ucichły, nic nie jest trzymane, pudełko nie
      // śpi i ucho nie drzemie — nadstaw ucho
      if (hal::earAvailable() && now > cooldownUntil_ && heldCount_ == 0 &&
          !dormant_ && !ambient::lullabyActive() &&
          now - lastKeyMs_ > kOpenAfterMs &&
          now - ModeInstrument::lastChimeMs() > 1500) {
        if (hal::earWindowOpen()) {
          duet::reset();
          st_ = St::Listen;
          heardVoice_ = false;
          listenStartMs_ = now;
          viz::setListening(true);
        } else {
          cooldownUntil_ = now + 1500;
        }
      }
      break;

    case St::Listen:
      // dobranoc w ŚPIEWIE: kołysanka nie może grać w zaparkowany głośnik
      if (ambient::lullabyActive()) {
        hal::earWindowClose();
        viz::setListening(false);
        viz::setVoiceLevel(0.0f);
        st_ = St::Idle;
        cooldownUntil_ = now + 2000;
        break;
      }
      // nikt nie śpiewa od dłuższego czasu: ucho zasypia, tampura wraca —
      // pudełko nie jest podsłuchem i nie udaje martwego do rana
      if (!heardVoice_ && now - listenStartMs_ > kListenTimeoutMs) {
        hal::earWindowClose();
        viz::setListening(false);
        st_ = St::Idle;
        dormant_ = true;  // do pierwszego klawisza
        Serial.println("grajek: spiew: ucho drzemie (nikt nie spiewal)");
        break;
      }
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
      // w rytmie głosu. Offsety ścieżki liczą się od otwarcia OKNA, a
      // odpowiedź zaczyna się od pojawienia GŁOSU — earAnswerOrigin()
      // spina oba zegary.
      const uint32_t played =
          hal::earAnswerOrigin() +
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
        firefly::note(comp, 0.4f);
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
