#include "mode_sing.h"

#include "../duet.h"
#include "../hal/audio_out.h"
#include "../viz.h"

void ModeSing::enter(ModeCtx& ctx) {
  ModeInstrument::enter(ctx);
  duet::reset();
  hal::earSetOpen(true);
  viz::setListening(hal::earAvailable());
  // bez ucha (full-duplex nie wstał) tryb dalej gra — tylko nie słyszy
  viz::toast(hal::earAvailable() ? "spiewaj!" : "ucho spi :(");
}

void ModeSing::exit(ModeCtx& ctx) {
  hal::earSetOpen(false);
  viz::setListening(false);
  duet::humOff(ctx.engine);
  ModeInstrument::exit(ctx);
}

void ModeSing::tick(ModeCtx& ctx, float dt) {
  ModeInstrument::tick(ctx, dt);
  duet::tick(ctx.engine);
  viz::setVoiceLevel(hal::earLevel() * 2.5f);  // pierścień słyszy głos
}
