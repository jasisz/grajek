#include "viz.h"

#include <Arduino.h>
#include <math.h>
#include <string.h>

#include "ambient.h"

namespace {

// --- geometria sceny ---
constexpr int kW = 240;
constexpr int kH = 135;
constexpr int kGroundY = 118;  // linia łąki

// zakres skali ustawia tryb (setPitchRange); rozsądny start na wszelki wypadek
float s_centsLo = -100.0f, s_centsHi = 4600.0f;

float pitch01(float c) {
  float t = (c - s_centsLo) / (s_centsHi - s_centsLo);
  if (t < 0.0f) t = 0.0f;
  if (t > 1.0f) t = 1.0f;
  return t;
}

// X = miejsce nuty W OKTAWIE (zawinięte), nie w całym zakresie — każda skala
// rozlewa się na pełną szerokość ekranu, a oktawa tej samej nuty ląduje w tym
// samym miejscu: duch grający wspomnienie oktawę wyżej rozbłyska dokładnie
// nad swoim nasionkiem. Zawinięcie jest ROZCIĄGNIĘTE do najwyższego stopnia
// skali w oktawie (setPitchRange), bo np. pentatonika kończy się na 884 c
// i bez tego prawa ćwiartka ekranu byłaby zawsze pusta.
float s_foldMax = 1.0f;

int xFromCents(float c) {
  float oct = c / 1200.0f;
  float frac = (oct - floorf(oct)) / s_foldMax;
  if (frac > 1.0f) frac = 1.0f;
  return 8 + (int)(frac * 224.0f);
}

// wyżej brzmi = wyżej lata; pasmo lotu 100..30 px (globalna wysokość)
int yFromCents(float c) { return 100 - (int)(pitch01(c) * 70.0f); }

// kolor przyciemniany w miejscu — kanvas 8-bit kwantyzuje sam
uint16_t shade(uint8_t r, uint8_t g, uint8_t b, float k) {
  if (k < 0.0f) k = 0.0f;
  if (k > 1.0f) k = 1.0f;
  return M5Canvas::color565((uint8_t)(r * k), (uint8_t)(g * k),
                            (uint8_t)(b * k));
}

uint32_t s_rng = 0x9d2c5680u;
float rnd01() {
  s_rng ^= s_rng << 13;
  s_rng ^= s_rng >> 17;
  s_rng ^= s_rng << 5;
  return (float)(s_rng >> 8) * (1.0f / 16777216.0f);
}

// --- nuty w świecie (świetlik / planeta / ryba / wybuch — zależnie od scen) ---
struct Firefly {
  bool active = false;
  bool falling = false;
  int id = -1;
  float cents = 0.0f, x = 0.0f, y = 0.0f, vel = 0.0f, phase = 0.0f;
  float age = 0.0f;  // sekundy od zagrania — ekspansja ogni, płynięcie ryb
};
Firefly s_fly[16];

// --- iskierki-nasionka w łące (pamięć) ---
struct Seed {
  bool active = false;
  int16_t x = 0;
  float glow = 0.0f;  // 1 = świeżo rozbłysła, opada do tlącej się
};
constexpr int kSeedCap = 32;
Seed s_seed[kSeedCap];
int s_seedHead = 0;

// --- iskry (machanie, rozbłyski duchów) ---
struct Spark {
  bool active = false;
  float x = 0, y = 0, vx = 0, vy = 0, life = 0;
};
Spark s_spark[24];

// --- gwiazdy (stałe pozycje, mrugają z oddechem) ---
struct Star { int16_t x, y; float phase; };
Star s_star[18];
bool s_starsInit = false;

// --- trawa (stałe źdźbła, kołyszą się z wiatrem i przechyłem) ---
struct Blade { int16_t x; int8_t h; float phase; };
constexpr int kBlades = 30;
Blade s_blade[kBlades];

// --- spadająca gwiazda (rzadki podarunek w chwilach ciszy) ---
struct Meteor { bool active; float x, y, vx, vy, life; };
Meteor s_meteor = {false, 0, 0, 0, 0, 0};

float s_tilt = 0.0f;   // boczny (jasność)
float s_depth = 0.0f;  // do/od siebie (przestrzeń)
bool s_listening = false;
float s_voiceLvl = 0.0f;
int s_scene = 0;

// kometa głosu: gaśnie sama, gdy duet przestaje ją odświeżać
float s_voiceCents = 0.0f, s_voiceGlow = 0.0f;
uint32_t s_voiceMs = 0;
float s_voiceTrail[4];  // ostatnie pozycje X dla warkocza
int s_voiceTrailN = 0;
char s_toast[24] = {0};
float s_toastT = 0.0f;
uint32_t s_lastMs = 0;

void spawnSparks(int x, int y, int n, float up) {
  for (int i = 0; i < 24 && n > 0; ++i) {
    if (s_spark[i].active) continue;
    s_spark[i] = {true, (float)x, (float)y, (rnd01() - 0.5f) * 70.0f,
                  -up * (20.0f + 50.0f * rnd01()), 0.5f + 0.3f * rnd01()};
    --n;
  }
}

void dropSeed(int x) {
  Seed& s = s_seed[s_seedHead];
  s_seedHead = (s_seedHead + 1) % kSeedCap;
  s = {true, (int16_t)x, 1.0f};
}

}  // namespace

namespace viz {

const char* sceneName(int idx) {
  static const char* names[kSceneCount] = {"laka", "kosmos", "ocean", "ognie",
                                           "mandala"};
  return names[((idx % kSceneCount) + kSceneCount) % kSceneCount];
}

void setScene(int idx) {
  s_scene = ((idx % kSceneCount) + kSceneCount) % kSceneCount;
}

void reset() {
  for (auto& f : s_fly) f.active = false;
  for (auto& s : s_seed) s.active = false;
  for (auto& p : s_spark) p.active = false;
  s_seedHead = 0;
  s_tilt = 0.0f;
  s_depth = 0.0f;
  s_voiceLvl = 0.0f;
  s_voiceGlow = 0.0f;
  s_voiceMs = 0;
  s_voiceTrailN = 0;
  s_toastT = 0.0f;
  s_lastMs = millis();
  s_meteor.active = false;
  s_listening = false;
  // the audio garden is the source of truth and survives mode switches —
  // replant its memories as seeds, so a peek into the settings screen no
  // longer burns the meadow (call setPitchRange BEFORE reset: seed X
  // positions depend on the octave fold)
  const int n = ambient::gardenCount();
  for (int i = 0; i < n; ++i)
    dropSeed(xFromCents(ambient::gardenCents(i)));
  if (!s_starsInit) {
    s_starsInit = true;
    for (int i = 0; i < 18; ++i)
      s_star[i] = {(int16_t)(4 + (int)(rnd01() * 232.0f)),
                   (int16_t)(4 + (int)(rnd01() * 70.0f)), rnd01()};
    for (int i = 0; i < kBlades; ++i)
      s_blade[i] = {(int16_t)(3 + (i * 237) / kBlades + (int)(rnd01() * 5.0f)),
                    (int8_t)(4 + (int)(rnd01() * 6.0f)), rnd01()};
  }
}

void setPitchRange(float loCents, float hiCents, float foldMax01) {
  if (hiCents - loCents < 600.0f) hiCents = loCents + 600.0f;
  s_centsLo = loCents - 100.0f;
  s_centsHi = hiCents + 100.0f;
  s_foldMax = foldMax01 < 0.3f ? 0.3f : (foldMax01 > 1.0f ? 1.0f : foldMax01);
}

void noteOn(int id, float cents, float vel) {
  Firefly* slot = nullptr;
  for (auto& f : s_fly)
    if (f.active && f.id == id) { slot = &f; break; }
  if (!slot)
    for (auto& f : s_fly)
      if (!f.active) { slot = &f; break; }
  if (!slot)
    for (auto& f : s_fly)
      if (f.falling) { slot = &f; break; }
  if (!slot) return;
  *slot = {true, false, id, cents, (float)xFromCents(cents),
           (float)yFromCents(cents), vel, rnd01(), 0.0f};
}

void noteOff(int id) {
  for (auto& f : s_fly)
    if (f.active && f.id == id && !f.falling) f.falling = true;
}

void chime(float cents, float vel) {
  const int x = xFromCents(cents);
  const int y = yFromCents(cents);
  spawnSparks(x, y, 3 + (int)(vel * 5.0f), 1.0f);
  // krótki świetlik, żeby dzwonek miał ciałko, nie tylko iskry; ujemne id
  // nie kolidują z siatką — a counter instead of rng, so two fast chimes
  // can't draw the same id and steal each other's firefly
  static int s_chimeFlySeq = 0;
  const int flyId = -1 - (s_chimeFlySeq++ & 0x3fff);
  noteOn(flyId, cents, vel);
  noteOff(flyId);  // od razu opada — zostaje po nim nasionko
}

void ghost(float cents) {
  // wspomnienie: rozbłyśnij nasionko najbliższe wysokości ducha
  const int gx = xFromCents(cents);
  Seed* best = nullptr;
  int bestD = 10000;
  for (auto& s : s_seed) {
    if (!s.active) continue;
    const int d = abs((int)s.x - gx);
    if (d < bestD) { bestD = d; best = &s; }
  }
  if (best) {
    best->glow = 1.0f;
    spawnSparks(best->x, kGroundY - 2, 3, 0.6f);
  } else {
    spawnSparks(gx, kGroundY - 2, 3, 0.6f);
  }
}

void setTilt(float norm) { s_tilt = norm; }

void setDepth(float norm) { s_depth = norm; }

void voice(float cents, float lvl) {
  s_voiceCents = cents;
  s_voiceGlow = lvl < 0.25f ? 0.25f : (lvl > 1.0f ? 1.0f : lvl);
  s_voiceMs = millis();
}

void setListening(bool on) { s_listening = on; }

void setVoiceLevel(float lvl) {
  s_voiceLvl = lvl < 0.0f ? 0.0f : (lvl > 1.0f ? 1.0f : lvl);
}

void toast(const char* text) {
  if (!text) return;
  strncpy(s_toast, text, sizeof(s_toast) - 1);
  s_toast[sizeof(s_toast) - 1] = 0;
  s_toastT = 1.2f;
}

// --- sceny: wspólny świat (nuty, wspomnienia, gesty), różne przebrania ---

struct Frame { float t, dt, breathe, sound; };

// wspólna fizyka, bez rysowania; sparkGrav różni sceny (bąbelki lecą w górę)
void updateWorld(const Frame& fr, float sparkGrav) {
  for (auto& s : s_seed)
    if (s.active) s.glow += (0.22f - s.glow) * fminf(1.0f, fr.dt * 1.4f);

  for (auto& f : s_fly)
    if (f.active) f.age += fr.dt;

  for (auto& f : s_fly) {
    if (!f.active || !f.falling) continue;
    f.y += (kGroundY + 4 - f.y) * fminf(1.0f, fr.dt * 3.0f);
    f.x += sinf((fr.t * 0.8f + f.phase) * 6.2832f) * 12.0f * fr.dt;
    if (f.y > kGroundY - 2.0f) {  // doleciał: zostaje wspomnieniem
      dropSeed((int)f.x);
      f.active = false;
    }
  }

  for (auto& p : s_spark) {
    if (!p.active) continue;
    p.life -= fr.dt;
    if (p.life <= 0.0f) { p.active = false; continue; }
    p.vy += sparkGrav * fr.dt;
    p.x += p.vx * fr.dt;
    p.y += p.vy * fr.dt;
    if (p.y > kGroundY + 14 || p.y < -4.0f) p.active = false;
  }

  // spadająca gwiazda: rzadki podarunek, tylko gdy jest naprawdę cicho
  if (!s_meteor.active && fr.breathe > 0.85f && rnd01() < 0.002f) {
    const float fromLeft = rnd01() < 0.5f ? 1.0f : -1.0f;
    s_meteor = {true, fromLeft > 0.0f ? 10.0f : 230.0f,
                6.0f + rnd01() * 25.0f, fromLeft * (150.0f + rnd01() * 80.0f),
                55.0f + rnd01() * 30.0f, 1.0f};
  }
  if (s_meteor.active) {
    s_meteor.life -= fr.dt * 1.4f;
    s_meteor.x += s_meteor.vx * fr.dt;
    s_meteor.y += s_meteor.vy * fr.dt;
    if (s_meteor.life <= 0.0f || s_meteor.y > 90.0f) s_meteor.active = false;
  }
}

void drawMeteor(M5Canvas& g, uint8_t r, uint8_t gg, uint8_t b) {
  if (!s_meteor.active) return;
  for (int k = 0; k < 5; ++k) {
    const float back = (float)k * 0.016f;
    g.drawPixel((int)(s_meteor.x - s_meteor.vx * back),
                (int)(s_meteor.y - s_meteor.vy * back),
                shade(r, gg, b, s_meteor.life * (1.0f - 0.2f * k)));
  }
}

void drawSparkPixels(M5Canvas& g, uint8_t r, uint8_t gg, uint8_t b) {
  for (auto& p : s_spark)
    if (p.active)
      g.drawPixel((int)p.x, (int)p.y,
                  shade(r, gg, b, fminf(1.0f, p.life * 2.0f)));
}

// świąteczna paleta ogni: kolor z fazy świetlika
void festive(float phase, uint8_t& r, uint8_t& gg, uint8_t& b) {
  switch (((int)(phase * 7.0f)) & 3) {
    case 0: r = 255; gg = 90;  b = 70;  break;
    case 1: r = 255; gg = 210; b = 80;  break;
    case 2: r = 110; gg = 255; b = 120; break;
    default: r = 130; gg = 170; b = 255; break;
  }
}

// scena 0: NOCNA ŁĄKA (oryginał)
void drawLaka(M5Canvas& g, const Frame& fr) {
  for (auto& st : s_star) {
    const float tw = 0.35f + 0.30f * fr.breathe +
                     0.25f * sinf((fr.t * (0.3f + 0.4f * fr.breathe) +
                                   st.phase) * 6.2832f);
    g.drawPixel(st.x, st.y, shade(180, 190, 255, tw));
  }
  drawMeteor(g, 255, 250, 210);

  const int mx = 120 + (int)(s_tilt * 85.0f);
  const float bright = 0.45f + 0.5f * (s_tilt * 0.5f + 0.5f);
  g.fillCircle(mx, 20, 8, shade(255, 240, 200, bright));
  g.fillCircle(mx - 3, 18, 7, shade(255, 240, 200, bright * 0.75f));

  const int bgN = ambient::backgroundNoteCount();
  for (int i = 0; i < bgN; ++i) {
    const int x = xFromCents(ambient::backgroundNoteCents(i));
    const int r = 20 + (int)(4.0f * sinf((fr.t / 7.0f + i * 0.31f) * 6.2832f));
    g.fillCircle(x, kGroundY + 14, r, shade(24, 64, 44, 0.8f));
  }

  g.fillRect(0, kGroundY, kW, kH - kGroundY,
             shade(18, 46, 26, 0.55f + 0.45f * fr.sound));
  g.drawFastHLine(0, kGroundY, kW, shade(60, 130, 70, 0.7f + 0.3f * fr.sound));
  for (auto& b : s_blade) {
    const float sway =
        sinf((fr.t * 0.45f + b.phase) * 6.2832f) * 2.0f + s_tilt * 4.0f;
    g.drawLine(b.x, kGroundY + 1, b.x + (int)sway, kGroundY + 1 - b.h,
               shade(46, 110, 52, 0.55f + 0.35f * fr.sound));
  }

  for (auto& s : s_seed) {
    if (!s.active) continue;
    const float k = s.glow;
    if (k > 0.55f) {
      g.fillCircle(s.x, kGroundY + 5, 2, shade(255, 235, 120, k));
      g.drawPixel(s.x, kGroundY + 2, shade(255, 235, 120, k * 0.8f));
    } else {
      g.drawPixel(s.x, kGroundY + 5, shade(255, 220, 110, 0.35f + k));
    }
  }

  for (auto& f : s_fly) {
    if (!f.active) continue;
    if (f.falling) {
      const float k = 0.35f + 0.4f * (1.0f - (f.y / (float)kGroundY));
      g.fillCircle((int)f.x, (int)f.y, 2, shade(255, 230, 110, k));
    } else {
      const float puls =
          0.75f + 0.25f * sinf((fr.t * 2.2f + f.phase) * 6.2832f);
      const int r = 3 + (int)(2.0f * f.vel * puls);
      const int x = (int)(f.x + sinf((fr.t * 1.1f + f.phase) * 6.2832f) * 3.0f);
      const int y =
          (int)(f.y + sinf((fr.t * 0.7f + f.phase * 2.0f) * 6.2832f) * 2.0f);
      g.fillCircle(x, y, r + 2, shade(120, 90, 20, 0.35f * puls));
      g.fillCircle(x, y, r, shade(255, 235, 120, 0.85f + 0.15f * puls));
      g.fillCircle(x, y, 1, shade(255, 255, 220, 1.0f));
    }
  }
  drawSparkPixels(g, 255, 210, 90);
}

// scena 1: KOSMOS — kompozycja RADIALNA: żadnego horyzontu. W środku jądro
// (centrum tonalne), nuty KRĄŻĄ po orbitach (wyżej = dalej i szybciej),
// wspomnienia to gwiazdy w zewnętrznym pierścieniu. Przechył boczny obraca
// cały układ, do/od siebie POCHYLA PŁASZCZYZNĘ orbit (spłaszcza elipsy).
void drawKosmos(M5Canvas& g, const Frame& fr) {
  constexpr int cx = 120, cy = 66;
  const float squash = 0.28f + 0.72f * (1.0f - fabsf(s_depth));
  const float spin = s_tilt * 1.4f;

  for (auto& st : s_star) {
    const float tw = 0.35f + 0.35f * fr.breathe +
                     0.2f * sinf((fr.t * 0.5f + st.phase) * 6.2832f);
    g.drawPixel(st.x, st.y, shade(190, 200, 255, tw));
    if (st.y + 68 < kH)  // the mirrored star must not fall off the bottom
      g.drawPixel(st.x, st.y + 68, shade(190, 200, 255, tw * 0.8f));
  }
  drawMeteor(g, 220, 240, 255);

  // orbity nut tła — pełne, spłaszczone elipsy wokół jądra
  const int bgN = ambient::backgroundNoteCount();
  for (int i = 0; i < bgN; ++i) {
    const float p01 = pitch01(ambient::backgroundNoteCents(i));
    const int rr = 22 + (int)(p01 * 44.0f);
    g.drawEllipse(cx, cy, rr, 1 + (int)(rr * squash),
                  shade(70, 90, 160, 0.35f));
  }

  // jądro pulsuje z dźwiękiem — serce układu
  const float core = 0.55f + 0.45f * fr.sound;
  g.fillCircle(cx, cy, 5 + (int)(3.0f * fr.sound), shade(255, 210, 120, core));
  g.fillCircle(cx, cy, 2, shade(255, 255, 230, 1.0f));

  // gwiazdy-wspomnienia w zewnętrznym pierścieniu (kąt z pozycji nuty)
  for (auto& s : s_seed) {
    if (!s.active) continue;
    const float a = ((float)s.x / 240.0f) * 6.2832f + spin;
    const int sx = cx + (int)(cosf(a) * 74.0f);
    const int sy = cy + (int)(sinf(a) * 74.0f * squash);
    const float k = s.glow;
    g.drawPixel(sx, sy, shade(235, 240, 255, 0.45f + k * 0.5f));
    if (k > 0.5f) {  // duch gra: gwiazda flaruje krzyżykiem
      g.drawPixel(sx - 2, sy, shade(235, 240, 255, k * 0.7f));
      g.drawPixel(sx + 2, sy, shade(235, 240, 255, k * 0.7f));
      g.drawPixel(sx, sy - 2, shade(235, 240, 255, k * 0.7f));
      g.drawPixel(sx, sy + 2, shade(235, 240, 255, k * 0.7f));
    }
  }

  // planety: krążą, wyżej = dalej i szybciej; za każdą smuga ze śladu orbity
  for (auto& f : s_fly) {
    if (!f.active) continue;
    uint8_t cr, cg, cb;
    festive(f.phase, cr, cg, cb);
    const float p01 = pitch01(f.cents);
    const float rr = 22.0f + p01 * 46.0f;
    const float ang =
        f.phase * 6.2832f + fr.t * (0.10f + 0.42f * p01) + spin;
    // gasnąca planeta po zwolnieniu klawisza (wysokość lotu = zanik)
    const float fade =
        f.falling ? fmaxf(0.0f, (kGroundY - f.y) / 60.0f) : 1.0f;
    if (fade <= 0.02f) continue;
    for (int k = 3; k >= 1; --k) {  // ogon: trzy punkty za planetą
      const float a2 = ang - (float)k * 0.13f;
      g.drawPixel(cx + (int)(cosf(a2) * rr),
                  cy + (int)(sinf(a2) * rr * squash),
                  shade(cr, cg, cb, fade * (0.30f - 0.07f * k)));
    }
    const int px = cx + (int)(cosf(ang) * rr);
    const int py = cy + (int)(sinf(ang) * rr * squash);
    const int r = 2 + (int)(2.5f * f.vel);
    g.fillCircle(px, py, r, shade(cr, cg, cb, 0.95f * fade));
    g.drawEllipse(px, py, r + 3, 1 + r / 2, shade(cr, cg, cb, 0.35f * fade));
    g.drawPixel(px - 1, py - 1, shade(255, 255, 255, 0.9f * fade));
  }
  drawSparkPixels(g, 210, 225, 255);
}

// scena 2: OCEAN — kompozycja PRZEPŁYWU: ryby płyną W POPRZEK ekranu (nie
// stoją!), zawijają się na krawędziach, kierunek z fazy nuty. Cała toń falami
// świetlnymi, tafla u góry. Przechył pochyla światło i wodorosty.
void drawOcean(M5Canvas& g, const Frame& fr) {
  // toń: pasy głębokości
  g.fillRect(0, 0, kW, 26, shade(30, 90, 120, 0.85f + 0.15f * fr.breathe));
  g.fillRect(0, 26, kW, 62, shade(14, 52, 88, 0.95f));
  g.fillRect(0, 88, kW, kGroundY - 88, shade(8, 30, 60, 1.0f));

  // tafla: falująca linia + refleksy
  for (int x = 0; x < kW; x += 2) {
    const int y = 4 + (int)(3.0f * sinf((x * 0.05f + fr.t * 0.9f)) +
                            2.0f * sinf((x * 0.11f - fr.t * 0.6f)));
    g.drawPixel(x, y, shade(190, 240, 255, 0.7f));
    g.drawPixel(x, y + 1, shade(140, 200, 235, 0.4f));
  }

  // kaustyki: skośne pasma światła (kąt z przechyłu do/od siebie)
  for (int i = 0; i < 5; ++i) {
    const float ph = fr.t * 0.25f + i * 1.3f;
    const int x0 = (int)(fmodf(ph * 40.0f + i * 60.0f, 300.0f)) - 30;
    const int lean = 26 + (int)(s_depth * 22.0f);
    for (int y = 8; y < 92; y += 3)
      g.drawPixel(x0 + (y * lean) / 60, y,
                  shade(150, 230, 255, 0.12f + 0.06f * sinf(ph + y * 0.1f)));
  }

  for (auto& st : s_star) {  // plankton dryfuje z prądem
    const int px = (int)(fmodf((float)st.x + fr.t * 6.0f, 240.0f));
    const float tw = 0.15f + 0.2f * sinf((fr.t * 0.7f + st.phase) * 6.2832f);
    g.drawPixel(px, 20 + st.y / 2, shade(160, 240, 230, tw));
  }

  // dno: piasek, koralowce tła, wodorosty kołyszące się z przechyłem
  g.fillRect(0, kGroundY, kW, kH - kGroundY,
             shade(140, 115, 70, 0.5f + 0.25f * fr.sound));
  const int bgN = ambient::backgroundNoteCount();
  for (int i = 0; i < bgN; ++i) {
    const int x = xFromCents(ambient::backgroundNoteCents(i));
    const float p = 0.45f + 0.2f * sinf((fr.t / 6.0f + i * 0.4f) * 6.2832f);
    g.fillCircle(x, kGroundY + 7, 7, shade(210, 100, 110, p));
    g.fillCircle(x - 5, kGroundY + 10, 4, shade(210, 100, 110, p * 0.8f));
  }
  for (auto& b : s_blade) {
    const float sway =
        sinf((fr.t * 0.35f + b.phase) * 6.2832f) * 5.0f + s_tilt * 8.0f;
    const int h = 6 + b.h * 2;
    g.drawLine(b.x, kGroundY + 2, b.x + (int)sway, kGroundY + 2 - h,
               shade(40, 150, 95, 0.55f));
    g.drawLine(b.x + (int)sway, kGroundY + 2 - h,
               b.x + (int)(sway * 1.6f), kGroundY - h - 4,
               shade(40, 150, 95, 0.4f));
  }

  // perły wspomnień na piasku
  for (auto& s : s_seed) {
    if (!s.active) continue;
    const float k = s.glow;
    if (k > 0.5f) {
      g.fillCircle(s.x, kGroundY + 4, 2, shade(255, 245, 235, k));
      g.drawCircle(s.x, kGroundY + 4, 4, shade(190, 235, 255, k * 0.5f));
    } else {
      g.drawPixel(s.x, kGroundY + 4, shade(235, 225, 215, 0.4f + k));
    }
  }

  // RYBY: płyną w poprzek, x rośnie z wiekiem nuty i zawija się
  for (auto& f : s_fly) {
    if (!f.active) continue;
    uint8_t cr, cg, cb;
    festive(f.phase, cr, cg, cb);
    if (f.falling) {  // po nucie: perła tonie na dno
      g.fillCircle((int)f.x, (int)f.y, 1, shade(240, 230, 220, 0.7f));
      g.drawPixel((int)f.x, (int)f.y - 3, shade(200, 230, 255, 0.3f));
      continue;
    }
    const bool right = f.phase > 0.5f;
    const float speed = 26.0f + 40.0f * f.vel;
    float x = f.x + (right ? 1.0f : -1.0f) * speed * f.age;
    x = fmodf(fmodf(x, 264.0f) + 264.0f, 264.0f) - 12.0f;  // zawijanie
    const int y = (int)(f.y + sinf((fr.t * 1.6f + f.phase) * 6.2832f) * 3.0f);
    const int r = 3 + (int)(1.5f * f.vel);
    const int dir = right ? 1 : -1;
    g.fillCircle((int)x, y, r, shade(cr, cg, cb, 0.95f));
    g.fillTriangle((int)x - dir * r, y, (int)x - dir * (r + 5), y - 4,
                   (int)x - dir * (r + 5), y + 4, shade(cr, cg, cb, 0.75f));
    g.drawPixel((int)x + dir * (r - 1), y - 1, shade(15, 20, 40, 1.0f));
  }
  // bąbelki: kółeczka lecące w górę
  for (auto& p : s_spark)
    if (p.active)
      g.drawCircle((int)p.x, (int)p.y, 1,
                   shade(195, 235, 255, fminf(1.0f, p.life * 1.6f)));
}

// scena 3: SZTUCZNE OGNIE — kompozycja EKSPANSJI: nuta to pocisk, który
// ROZKWITA pierścieniem rosnącym w czasie (age), potem opada żarem. Ekran
// prawie pusty poza wybuchami — cała uwaga na nutach.
void drawOgnie(M5Canvas& g, const Frame& fr) {
  for (auto& st : s_star) {
    const float tw = 0.18f + 0.14f * sinf((fr.t * 0.4f + st.phase) * 6.2832f);
    g.drawPixel(st.x, st.y, shade(170, 180, 240, tw));
  }
  drawMeteor(g, 255, 250, 210);

  // ziemia: sama krecha + żar wspomnień; miasteczko tylko jako cienka rytmika
  g.drawFastHLine(0, kGroundY + 10, kW, shade(40, 34, 50, 1.0f));
  const int bgN = ambient::backgroundNoteCount();
  for (int i = 0; i < bgN; ++i) {
    const int x = xFromCents(ambient::backgroundNoteCents(i));
    const float p = 0.35f + 0.3f * sinf((fr.t / 6.0f + i * 0.37f) * 6.2832f);
    g.drawLine(x, kGroundY + 10, x, kGroundY + 4, shade(120, 90, 60, 0.5f));
    g.fillCircle(x, kGroundY + 2, 2, shade(255, 180, 90, p));  // lampion
  }
  for (auto& s : s_seed) {
    if (!s.active) continue;
    const float k = s.glow;
    if (k > 0.5f) {
      g.fillCircle(s.x, kGroundY + 8, 2, shade(255, 150, 60, k));
      g.drawPixel(s.x, kGroundY + 5, shade(255, 190, 90, k * 0.6f));
    } else {
      g.drawPixel(s.x, kGroundY + 8,
                  shade(255, 120, 50,
                        0.3f + k + 0.15f * sinf((fr.t * 1.3f + s.x) * 6.2832f)));
    }
  }

  // WYBUCHY: pierścień punktów o promieniu rosnącym z wiekiem nuty
  for (auto& f : s_fly) {
    if (!f.active) continue;
    uint8_t cr, cg, cb;
    festive(f.phase, cr, cg, cb);
    if (f.falling) {  // żar spada
      g.fillCircle((int)f.x, (int)f.y, 1, shade(255, 150, 60, 0.85f));
      g.drawPixel((int)f.x, (int)f.y - 4, shade(255, 120, 50, 0.35f));
      continue;
    }
    const float maxR = 16.0f + 26.0f * f.vel;
    const float grow = 1.0f - expf(-f.age * 4.5f);  // szybki rozkwit
    const float rr = maxR * grow;
    const int pts = 12;
    const float bright = 0.35f + 0.65f * (1.0f - grow * 0.75f);
    for (int k = 0; k < pts; ++k) {
      const float a = (f.phase + (float)k / pts) * 6.2832f;
      const int px = (int)(f.x + cosf(a) * rr);
      const int py = (int)(f.y + sinf(a) * rr * 0.9f);
      g.fillCircle(px, py, rr > 22.0f ? 1 : 2, shade(cr, cg, cb, bright));
      // iskra ciągnąca się do środka — smuga wybuchu
      g.drawPixel((int)(f.x + cosf(a) * rr * 0.72f),
                  (int)(f.y + sinf(a) * rr * 0.65f),
                  shade(cr, cg, cb, bright * 0.45f));
    }
    // drugi, wolniejszy pierścień dla mocnych nut
    if (f.vel > 0.6f) {
      const float rr2 = maxR * (1.0f - expf(-f.age * 2.2f)) * 0.55f;
      for (int k = 0; k < 8; ++k) {
        const float a = (f.phase * 2.0f + (float)k / 8.0f) * 6.2832f;
        g.drawPixel((int)(f.x + cosf(a) * rr2), (int)(f.y + sinf(a) * rr2),
                    shade(255, 245, 220, bright * 0.8f));
      }
    }
    g.fillCircle((int)f.x, (int)f.y, 2, shade(255, 255, 235, bright));
  }
  drawSparkPixels(g, 255, 190, 80);
}

// scena 4: MANDALA — czysta abstrakcja: brzmiące nuty tworzą symetryczny wzór
// wokół środka, cały obraca się wolno. Przechył boczny kręci szybciej,
// do/od siebie ZMIENIA KROTNOŚĆ SYMETRII (4..9 ramion) — od surowej do gęstej.
void drawMandala(M5Canvas& g, const Frame& fr) {
  constexpr int cx = 120, cy = 67;
  const int arms = 4 + (int)((s_depth * 0.5f + 0.5f) * 5.0f);  // 4..9
  const float rot = fr.t * 0.08f + s_tilt * 1.6f;

  // oddech: pierścienie tła pulsują z akordem
  const int bgN = ambient::backgroundNoteCount();
  for (int i = 0; i < bgN; ++i) {
    const float p01 = pitch01(ambient::backgroundNoteCents(i));
    const int rr = 16 + (int)(p01 * 46.0f);
    const float p = 0.18f + 0.12f * sinf((fr.t / 6.0f + i * 0.4f) * 6.2832f);
    g.drawCircle(cx, cy, rr, shade(90, 80, 160, p + 0.1f * fr.sound));
  }

  // wspomnienia: koraliki na obwodzie, każdy powielony symetrycznie
  for (auto& s : s_seed) {
    if (!s.active) continue;
    const float base = ((float)s.x / 240.0f) * 6.2832f + rot;
    const float k = s.glow;
    for (int a = 0; a < arms; ++a) {
      const float ang = base + (float)a * 6.2832f / arms;
      const int px = cx + (int)(cosf(ang) * 62.0f);
      const int py = cy + (int)(sinf(ang) * 62.0f);
      g.drawPixel(px, py, shade(200, 180, 255, 0.35f + k * 0.55f));
      if (k > 0.5f) g.drawCircle(px, py, 2, shade(220, 200, 255, k * 0.6f));
    }
  }

  // nuty: płatki powielone symetrycznie, promień z wysokości dźwięku
  for (auto& f : s_fly) {
    if (!f.active) continue;
    uint8_t cr, cg, cb;
    festive(f.phase, cr, cg, cb);
    const float fade =
        f.falling ? fmaxf(0.0f, (kGroundY - f.y) / 60.0f) : 1.0f;
    if (fade <= 0.02f) continue;
    const float p01 = pitch01(f.cents);
    const float rr = 14.0f + p01 * 46.0f;
    const float base = f.phase * 6.2832f + rot * (1.0f + p01);
    const float puls = 0.75f + 0.25f * sinf((fr.t * 2.0f + f.phase) * 6.2832f);
    const int size = 2 + (int)(3.0f * f.vel * puls);
    for (int a = 0; a < arms; ++a) {
      const float ang = base + (float)a * 6.2832f / arms;
      const int px = cx + (int)(cosf(ang) * rr);
      const int py = cy + (int)(sinf(ang) * rr);
      g.drawLine(cx + (int)(cosf(ang) * rr * 0.35f),
                 cy + (int)(sinf(ang) * rr * 0.35f), px, py,
                 shade(cr, cg, cb, 0.25f * fade * puls));
      g.fillCircle(px, py, size, shade(cr, cg, cb, 0.9f * fade));
      g.fillCircle(px, py, size > 2 ? 1 : 0, shade(255, 255, 255, 0.8f * fade));
    }
  }

  // środek: jasny punkt oddychający z dźwiękiem
  g.fillCircle(cx, cy, 3 + (int)(3.0f * fr.sound),
               shade(255, 230, 180, 0.5f + 0.5f * fr.sound));
  drawSparkPixels(g, 230, 210, 255);
}

void draw(M5Canvas& g) {
  const uint32_t now = millis();
  float dt = (float)(now - s_lastMs) * 0.001f;
  s_lastMs = now;
  if (dt > 0.1f) dt = 0.1f;
  const Frame fr{(float)now * 0.001f, dt, ambient::breathe01(),
                 1.0f - ambient::breathe01()};
  const float t = fr.t;

  // bąbelki w oceanie płyną w górę, żar ogni spada ciężko, w kosmosie dryf
  static constexpr float kSparkGrav[kSceneCount] = {90.0f, 12.0f, -70.0f,
                                                    130.0f, 25.0f};
  updateWorld(fr, kSparkGrav[s_scene]);

  g.fillSprite(TFT_BLACK);
  switch (s_scene) {
    case 0: drawLaka(g, fr); break;
    case 1: drawKosmos(g, fr); break;
    case 2: drawOcean(g, fr); break;
    case 3: drawOgnie(g, fr); break;
    default: drawMandala(g, fr); break;
  }

  // kometa głosu: chłodny błękit (kontrast z ciepłymi świetlikami),
  // leci na wysokości śpiewu z krótkim warkoczem, gaśnie po ~0.4 s ciszy
  if (s_voiceMs != 0 && now - s_voiceMs < 400) {
    const float k = s_voiceGlow * (1.0f - (float)(now - s_voiceMs) / 400.0f);
    const int vx = xFromCents(s_voiceCents);
    const int vy = yFromCents(s_voiceCents);
    if (s_voiceTrailN == 0 || fabsf(s_voiceTrail[0] - (float)vx) > 1.5f) {
      for (int i = 3; i > 0; --i) s_voiceTrail[i] = s_voiceTrail[i - 1];
      s_voiceTrail[0] = (float)vx;
      if (s_voiceTrailN < 4) ++s_voiceTrailN;
    }
    for (int i = 1; i < s_voiceTrailN; ++i)
      g.drawPixel((int)s_voiceTrail[i], vy,
                  shade(150, 200, 255, k * (1.0f - 0.25f * i)));
    g.fillCircle(vx, vy, 2 + (int)(3.0f * k), shade(120, 180, 255, 0.4f * k));
    g.fillCircle(vx, vy, 1 + (int)(1.5f * k), shade(200, 230, 255, k));
  } else {
    s_voiceTrailN = 0;
  }

  // ucho otwarte: pulsujący pierścień w lewym górnym rogu; rośnie i
  // jaśnieje z głosem — "pudełko mnie słyszy" widać od razu
  if (s_listening) {
    const float p = 0.5f + 0.5f * sinf(t * 1.2f * 6.2832f);
    const int r = 5 + (int)(2.0f * p + 8.0f * s_voiceLvl);
    g.drawCircle(14, 14, r,
                 shade(255, 150, 90, 0.5f + 0.4f * p + 0.3f * s_voiceLvl));
    g.fillCircle(14, 14, 2 + (int)(2.0f * s_voiceLvl),
                 shade(255, 190, 120, 0.9f));
  }

  // toast: jedyny tekst na scenie, znika sam
  if (s_toastT > 0.0f) {
    s_toastT -= dt;
    const float k = s_toastT > 0.3f ? 1.0f : s_toastT / 0.3f;
    g.setTextSize(2);
    g.setTextColor(shade(255, 200, 60, k), TFT_BLACK);
    const int w = (int)strlen(s_toast) * 12;
    g.drawString(s_toast, (kW - w) / 2, 44);
  }
}

}  // namespace viz
