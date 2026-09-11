#include "pet.h"

#include <array>

// ---------------------------------------------------------------------------
// utils/math.ts
// ---------------------------------------------------------------------------

namespace {

inline float clampf(float v, float lo = 0.0f, float hi = 1.0f) {
  return v < lo ? lo : (v > hi ? hi : v);
}
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
inline float easeOutQuint(float t) {
  float u = 1.0f - t;
  return 1.0f - u * u * u * u * u;
}
inline float easeOutCubic(float t) {
  float u = 1.0f - t;
  return 1.0f - u * u * u;
}

float loopNoise(float t, float period, float seed) {
  float p = (t / period) * TWO_PI;
  return 0.55f * sinf(p + seed)
       + 0.30f * sinf(2.0f * p + seed * 1.7f + 1.1f)
       + 0.15f * sinf(3.0f * p + seed * 2.3f + 2.4f);
}

// createRng — mulberry32-style, bit-exact port
uint32_t rngState;
float rngNext() {
  rngState += 0x6D2B79F5u;
  uint32_t t = rngState;
  t = (t ^ (t >> 15)) * (1u | t);
  uint32_t prod = (t ^ (t >> 7)) * (61u | t);
  t = (t + prod) ^ t;
  return static_cast<float>(t ^ (t >> 14)) / 4294967296.0f;
}

// ---------------------------------------------------------------------------
// profiles.ts — exact radial profiles, 64 samples.
// theta = 0 points right, grows clockwise (y-down screen coords).
// GHOST_PROFILES.hem/.curl/.arms are copied verbatim from profiles.ts
// (generated from the ghost IP artwork by tools/radial_convert.py, max radius
// normalized to 0.55). Do not hand-edit; regenerate from the app repo.
// ---------------------------------------------------------------------------

const float GHOST_HEM[FinagotchiPet::NRAD] = {
  0.3912f, 0.3923f, 0.3965f, 0.4039f, 0.4144f, 0.4281f, 0.4451f, 0.4662f,
  0.4900f, 0.5104f, 0.5190f, 0.5092f, 0.4776f, 0.4559f, 0.4468f, 0.4538f,
  0.4466f, 0.4399f, 0.4479f, 0.4681f, 0.4849f, 0.4837f, 0.4959f, 0.5184f,
  0.5477f, 0.5500f, 0.5180f, 0.4687f, 0.4199f, 0.3910f, 0.3763f, 0.3691f,
  0.3670f, 0.3687f, 0.3733f, 0.3809f, 0.3914f, 0.4045f, 0.4186f, 0.4329f,
  0.4464f, 0.4592f, 0.4708f, 0.4816f, 0.4910f, 0.4988f, 0.5047f, 0.5087f,
  0.5113f, 0.5121f, 0.5110f, 0.5079f, 0.5033f, 0.4965f, 0.4881f, 0.4778f,
  0.4666f, 0.4546f, 0.4418f, 0.4283f, 0.4157f, 0.4051f, 0.3978f, 0.3931f,
};

const float GHOST_CURL[FinagotchiPet::NRAD] = {
  0.4336f, 0.4319f, 0.4317f, 0.4331f, 0.4359f, 0.4401f, 0.4447f, 0.4495f,
  0.4544f, 0.4586f, 0.4619f, 0.4640f, 0.4657f, 0.4668f, 0.4680f, 0.4693f,
  0.4718f, 0.4758f, 0.4819f, 0.4899f, 0.4998f, 0.5109f, 0.5231f, 0.5342f,
  0.5439f, 0.5500f, 0.5477f, 0.4891f, 0.4302f, 0.3783f, 0.3819f, 0.3878f,
  0.3947f, 0.4031f, 0.4117f, 0.4203f, 0.4289f, 0.4376f, 0.4460f, 0.4535f,
  0.4609f, 0.4678f, 0.4743f, 0.4798f, 0.4844f, 0.4882f, 0.4914f, 0.4937f,
  0.4956f, 0.4962f, 0.4966f, 0.4962f, 0.4951f, 0.4926f, 0.4895f, 0.4853f,
  0.4800f, 0.4739f, 0.4672f, 0.4605f, 0.4531f, 0.4464f, 0.4407f, 0.4365f,
};

const float GHOST_ARMS[FinagotchiPet::NRAD] = {
  0.4486f, 0.4351f, 0.4082f, 0.3940f, 0.4023f, 0.4486f, 0.5026f, 0.5448f,
  0.5500f, 0.5282f, 0.4971f, 0.4683f, 0.4467f, 0.4310f, 0.4203f, 0.4141f,
  0.4122f, 0.4141f, 0.4203f, 0.4310f, 0.4467f, 0.4663f, 0.4834f, 0.4915f,
  0.4869f, 0.4731f, 0.4541f, 0.4342f, 0.4161f, 0.4045f, 0.4139f, 0.4384f,
  0.4659f, 0.4484f, 0.4240f, 0.4004f, 0.4106f, 0.4213f, 0.4327f, 0.4436f,
  0.4539f, 0.4628f, 0.4705f, 0.4764f, 0.4810f, 0.4836f, 0.4851f, 0.4851f,
  0.4845f, 0.4825f, 0.4797f, 0.4757f, 0.4709f, 0.4642f, 0.4561f, 0.4462f,
  0.4362f, 0.4248f, 0.4128f, 0.3999f, 0.3879f, 0.3772f, 0.3975f, 0.4244f,
};

float gRadii[kPetStateCount][FinagotchiPet::NRAD];
bool  gTablesReady = false;

void buildTables() {
  if (gTablesReady) return;
  gTablesReady = true;

  for (int i = 0; i < FinagotchiPet::NRAD; i++) {
    float theta = static_cast<float>(i) / FinagotchiPet::NRAD * TWO_PI;
    float degrees = static_cast<float>(i) / FinagotchiPet::NRAD * 360.0f;
    float c = fabsf(cosf(theta));
    float s = fabsf(sinf(theta));

    // egg: superellipse, n 2.5 bottom / 1.8 top, wobble sin(3t) — unchanged
    float n = degrees < 180.0f ? 2.5f : 1.8f;
    float se = powf(powf(c, n) + powf(s, n), -1.0f / n);
    gRadii[static_cast<size_t>(PetState::PET_EGG)][i] = clampf(0.45f * se + 0.015f * sinf(3.0f * theta));
  }

  // Ghost silhouettes ride on the coinling/hodler/whale stages (engine.ts).
  memcpy(gRadii[static_cast<size_t>(PetState::PET_COINLING)], GHOST_HEM,  sizeof(GHOST_HEM));
  memcpy(gRadii[static_cast<size_t>(PetState::PET_HODLER)],   GHOST_CURL, sizeof(GHOST_CURL));
  memcpy(gRadii[static_cast<size_t>(PetState::PET_WHALE)],    GHOST_ARMS, sizeof(GHOST_ARMS));
}

} // namespace

// shape.ts radiusAtAngle: linear interpolation between nearest samples.
float FinagotchiPet::radiusAt(const float* radii, float theta) const {
  float t = theta * (static_cast<float>(NRAD) / TWO_PI);
  float tf = floorf(t);
  int i0 = (static_cast<int>(tf) % NRAD + NRAD) % NRAD;
  int i1 = (i0 + 1) % NRAD;
  return lerpf(radii[i0], radii[i1], t - tf);
}

// ---------------------------------------------------------------------------
// face.ts — blink schedule, blinkScale, eyePoses.
// ---------------------------------------------------------------------------

namespace {

float gBlinks[512];
int   gBlinkCount = 0;
const float BLINK_DUR = 0.18f;

void buildBlinks() {
  rngState = 0x5eedu;
  gBlinkCount = 0;
  float t = 1.4f;
  while (t < 900.0f && gBlinkCount < 510) {
    gBlinks[gBlinkCount++] = t;
    t += 1.9f + rngNext() * 2.7f;
    if (rngNext() < 0.18f) {           // occasional double blink
      gBlinks[gBlinkCount++] = t;
      t += 0.24f;
    }
  }
}

float blinkLid(float t) {
  t = fmodf(t, 900.0f);   // wrap: the web schedule ends at 900 s; loop it
  for (int i = 0; i < gBlinkCount; i++) {
    float start = gBlinks[i];
    if (t < start) break;
    float k = (t - start) / BLINK_DUR;
    if (k >= 0.0f && k <= 1.0f) {
      return k < 0.45f ? 1.0f - k / 0.45f : (k - 0.45f) / 0.55f;
    }
  }
  return 1.0f;
}

inline float blinkScale(float lid) { return 0.06f + 0.94f * clampf(lid); }

// Rotate two vectors in their common plane (face.ts spin).
void spin3(const float u[3], const float v[3], float angle,
           float outU[3], float outV[3]) {
  float c = cosf(angle), s = sinf(angle);
  for (int i = 0; i < 3; i++) {
    outU[i] = u[i] * c + v[i] * s;
    outV[i] = v[i] * c - u[i] * s;
  }
}

// face.ts eyePoses. Screen coords: x right, y down, z toward viewer.
// Index 0 = inner eye, index 1 = outer eye.
void eyePoses(float yawDeg, float pitchDeg, float rollDeg,
              float scale, float splitDeg, FinagotchiPet::EyePose out[2]) {
  float fwd[3] = {0, 0, 1}, rgt[3] = {1, 0, 0}, dwn[3] = {0, 1, 0};
  float t1[3], t2[3];

  spin3(fwd, rgt, yawDeg * DEG_TO_RAD, t1, t2);
  memcpy(fwd, t1, sizeof(t1)); memcpy(rgt, t2, sizeof(t2));
  spin3(dwn, fwd, pitchDeg * DEG_TO_RAD, t1, t2);
  memcpy(dwn, t1, sizeof(t1)); memcpy(fwd, t2, sizeof(t2));
  spin3(rgt, dwn, rollDeg * DEG_TO_RAD, t1, t2);
  memcpy(rgt, t1, sizeof(t1)); memcpy(dwn, t2, sizeof(t2));

  for (int i = 0; i < 2; i++) {
    float side = (i == 0) ? -1.0f : 1.0f;
    float ef[3], er[3];
    spin3(fwd, rgt, splitDeg * side * DEG_TO_RAD, ef, er);
    out[i].x = ef[0] * scale;
    out[i].y = ef[1] * scale;
    out[i].a = er[0]; out[i].b = er[1];
    out[i].c = dwn[0]; out[i].d = dwn[1];
    out[i].depth = ef[2];
  }
}

// shape.ts capsulePath sampled: stadium boundary, axis along the longer side.
int capsulePoints(float w, float h, float* xs, float* ys, int maxN) {
  float r = fminf(w, h) * 0.5f;
  float L = fabsf(w - h) * 0.5f;
  bool horizontal = w >= h;
  const int H = 9;   // samples per semicircle
  int n = 0;
  for (int i = 0; i < H && n < maxN; i++) {
    float a = (-90.0f + 180.0f * static_cast<float>(i) / static_cast<float>(H - 1)) * DEG_TO_RAD;
    float px = L + r * cosf(a), py = r * sinf(a);
    xs[n] = horizontal ? px : py;
    ys[n] = horizontal ? py : px;
    n++;
  }
  for (int i = 0; i < H && n < maxN; i++) {
    float a = (90.0f + 180.0f * static_cast<float>(i) / static_cast<float>(H - 1)) * DEG_TO_RAD;
    float px = -L + r * cosf(a), py = r * sinf(a);
    xs[n] = horizontal ? px : py;
    ys[n] = horizontal ? py : px;
    n++;
  }
  return n;
}

// ---------------------------------------------------------------------------
// engine.ts STATE_DEFS + expressions.ts EXPRESSIONS
// ---------------------------------------------------------------------------

struct EyeDef { float w, h, tilt, open; };

struct StageDef {
  float offX, offY;
  float gazeYaw, gazePitch, gazeRoll;
  float split;
  EyeDef eyes[2];
  float morph;
  uint8_t fr, fg, fb;
  bool    hasGlow;
  uint8_t gr, gg, gb;
};

const StageDef DEFS[kPetStateCount] = {
  // egg
  { 0.0f,  0.04f,  0.0f,  18.0f, 0.0f, 12.0f,
    {{ 0.16f, 0.06f, 0.0f, 1.0f }, { 0.16f, 0.06f, 0.0f, 1.0f }}, 0.45f,
    0xD4, 0xC8, 0xB8, false, 0, 0, 0 },
  // coinling
  { 0.0f,  0.00f,  0.0f,  -8.0f, 0.0f, 16.0f,
    {{ 0.22f, 0.22f, 0.0f, 1.0f }, { 0.22f, 0.22f, 0.0f, 1.0f }}, 0.45f,
    0xF5, 0x9E, 0x0B, true,  0xFB, 0xBF, 0x24 },
  // hodler
  { 0.0f, -0.03f,  0.0f,   2.0f, 0.0f, 14.0f,
    {{ 0.22f, 0.09f, 0.0f, 1.0f }, { 0.22f, 0.09f, 0.0f, 1.0f }}, 0.45f,
    0x3B, 0x82, 0xF6, true,  0x60, 0xA5, 0xFA },
  // whale
  { 0.0f,  0.05f,  0.0f, -12.0f, 0.0f, 13.0f,
    {{ 0.14f, 0.09f, 0.0f, 1.0f }, { 0.14f, 0.09f, 0.0f, 1.0f }}, 0.55f,
    0x63, 0x66, 0xF1, true,  0x81, 0x8C, 0xF8 },
};

// expressions.ts EXPRESSIONS — absolute gaze/split/eye overrides.
// pair() mirrors tilt: eyes[0].tilt = +tilt, eyes[1].tilt = -tilt.
// calm uses face.ts defaults: REST_GAZE (0,-12,0), EYE_SPLIT 19, EYE_W/H .2/.28.
struct ExprDef {
  float gazeYaw, gazePitch, gazeRoll;
  float split;
  EyeDef eyes[2];
};

const ExprDef EXPR[kMoodCount] = {
  /* calm    */ {  0.0f, -12.0f,  0.0f, 19.0f,
                  {{ 0.2f, 0.28f, 0.0f, 1.0f }, { 0.2f, 0.28f, 0.0f, 1.0f }} },
  /* happy   */ {  2.0f, -10.0f,  0.0f, 19.5f,
                  {{ 0.24f, 0.15f, 18.0f, 1.0f }, { 0.24f, 0.15f, -18.0f, 1.0f }} },
  /* excited */ {  4.0f, -16.0f,  0.0f, 20.5f,
                  {{ 0.3f, 0.34f, -8.0f, 1.0f }, { 0.3f, 0.34f, 8.0f, 1.0f }} },
  /* waiting */ { -8.0f,  -4.0f, -6.0f, 18.5f,
                  {{ 0.22f, 0.32f, -6.0f, 1.0f }, { 0.22f, 0.32f, 6.0f, 1.0f }} },
  /* sleepy  */ {  0.0f,  -4.0f,  0.0f, 18.0f,
                  {{ 0.2f, 0.34f, 0.0f, 0.42f }, { 0.2f, 0.34f, 0.0f, 0.42f }} },
  /* sad     */ {  2.0f,  -2.0f,  0.0f, 18.5f,
                  {{ 0.22f, 0.32f, -24.0f, 1.0f }, { 0.22f, 0.32f, 24.0f, 1.0f }} },
};

// PetCanvas reactions as keyframe tracks {time, scale, rotDeg}.
struct ReactKey { float t, s, r; };
const std::array<ReactKey, 4> KEYS_JUMP  = {{{0,1,0},{0.22f,1.28f,0},{0.45f,0.88f,0},{0.8f,1,0}}};
const std::array<ReactKey, 4> KEYS_SPIN  = {{{0,1,0},{0.12f,0.9f,0},{0.65f,1.08f,360},{0.9f,1,360}}};
const std::array<ReactKey, 7> KEYS_GLOW  = {{{0,1,0},{0.16f,1.16f,0},{0.34f,1.04f,0},{0.52f,1.14f,0},
                                             {0.70f,1.04f,0},{0.88f,1.12f,0},{1.08f,1,0}}};
const std::array<ReactKey, 9> KEYS_DANCE = {{{0,1,0},{0.11f,1.06f,-14},{0.22f,0.94f,14},{0.33f,1.06f,-14},
                                             {0.44f,0.94f,14},{0.55f,1.06f,-14},{0.66f,0.94f,14},
                                             {0.77f,1.04f,-8},{0.95f,1,0}}};

} // namespace

// ---------------------------------------------------------------------------
// FinagotchiPet
// ---------------------------------------------------------------------------

void FinagotchiPet::begin(TFT_eSPI* display, float scale) {
  tft = display;
  R = scale;
  buildTables();
  buildBlinks();
  spr.reset(new TFT_eSprite(tft));
  spr->setColorDepth(16);
  if (spr->createSprite(tft->width(), tft->height()) == nullptr) {
    spr->createSprite(200, 200);   // RAM fallback
  }
}

void FinagotchiPet::end() {
  spr.reset();
}

void FinagotchiPet::setState(PetState id, float nowSec) {
  if (id == cur || id >= PetState::PET_STATE_COUNT) return;
  if (id > cur) burstT = nowSec;   // LevelUpAnimation trigger (stage up only)
  fromPose = poseAt(nowSec);       // departFige: morph from the visible pose
  hasFrom = true;
  cur = id;
  tCur = nowSec;
}

bool FinagotchiPet::evolve(float nowSec) {
  if (cur >= PetState::PET_WHALE) return false;
  setState(static_cast<PetState>(static_cast<uint8_t>(cur) + 1), nowSec);
  return true;
}

void FinagotchiPet::setLook(float yawDeg, float pitchDeg, float nowSec) {
  float k = clampf((nowSec - lookAtT) / 0.24f);
  float e = easeOutQuint(k);
  lookPrevYaw = lerpf(lookPrevYaw, lookYaw, e);
  lookPrevPitch = lerpf(lookPrevPitch, lookPitch, e);
  lookPrevMix = lerpf(lookPrevMix, lookMix, e);
  lookPrevWander = lerpf(lookPrevWander, lookWander, e);

  lookYaw = clampf(yawDeg, -30.0f, 30.0f);
  lookPitch = clampf(pitchDeg, -25.0f, 25.0f);
  lookMix = 0.85f;
  lookWander = 0.0f;
  lookAtT = nowSec;
}

void FinagotchiPet::clearLook(float nowSec) {
  float k = clampf((nowSec - lookAtT) / 0.24f);
  float e = easeOutQuint(k);
  lookPrevYaw = lerpf(lookPrevYaw, lookYaw, e);
  lookPrevPitch = lerpf(lookPrevPitch, lookPitch, e);
  lookPrevMix = lerpf(lookPrevMix, lookMix, e);
  lookPrevWander = lerpf(lookPrevWander, lookWander, e);

  lookYaw = 0.0f;
  lookPitch = 0.0f;
  lookMix = 0.0f;
  lookWander = 1.0f;
  lookAtT = nowSec;
}

void FinagotchiPet::setMood(PetMoodId m, float nowSec) {
  if (m >= PetMoodId::MOOD_COUNT || m == curMood) return;
  prevMood = curMood;
  curMood = m;
  moodAtT = nowSec;
}

void FinagotchiPet::setItem(PetItem i) {
  // Unknown ids (from a newer app) degrade to none instead of misrendering.
  curItem = i < PetItem::ITEM_COUNT ? i : PetItem::ITEM_NONE;
}

void FinagotchiPet::react(PetReaction r, float nowSec) {
  if (r > PetReaction::REACT_DANCE) return;
  reaction = r;
  reactT = nowSec;
}

void FinagotchiPet::setStats(uint32_t streakDays, uint32_t points,
                             uint8_t happiness) {
  statsStreak = streakDays;
  statsPoints = points;
  statsHappy = happiness > 100 ? 100 : happiness;
}

void FinagotchiPet::setBattery(uint8_t pct) {
  batteryPct = pct > 100 ? 100 : pct;
  batteryKnown = true;
}

void FinagotchiPet::clearBattery() {
  batteryKnown = false;
}

void FinagotchiPet::setSyncWait(bool on, float nowSec) {
  if (on == syncWait) return;
  syncWait = on;
  if (on) {
    preSyncMood = curMood;
    setMood(PetMoodId::MOOD_WAITING, nowSec);
  } else {
    setMood(preSyncMood, nowSec);
  }
}

// ---------------------------------------------------------------------------
// Bottom stats bar: flame + streak, sparkle + points, heart + happiness.
// ---------------------------------------------------------------------------

namespace {

void fmtVal(uint32_t v, char* buf, size_t n) {
  if (v >= 100000)   snprintf(buf, n, "%luk", static_cast<unsigned long>(v / 1000));
  else if (v >= 10000) snprintf(buf, n, "%.1fk", v / 1000.0f);
  else               snprintf(buf, n, "%lu", static_cast<unsigned long>(v));
}

} // namespace

void FinagotchiPet::drawStatsBar() {
  int w = spr->width(), h = spr->height();
  int barY = h - 56;                   // extra room below for the sync caption
  int cy = barY + 19;                  // icon/text vertical center

  uint16_t dim = spr->color565(45, 45, 60);
  uint16_t txt = spr->color565(230, 230, 240);
  spr->drawFastHLine(10, barY, w - 20, dim);

  spr->setTextDatum(ML_DATUM);
  spr->setTextSize(2);
  spr->setTextColor(txt, spr->color565(0x07, 0x11, 0x1F));

  const int cols[3] = { w / 6, w / 2, w * 5 / 6 };
  char buf[12];

  // --- streak: flame ---
  fmtVal(statsStreak, buf, sizeof(buf));
  {
    int tw = spr->textWidth(buf);
    int gx = cols[0] - (16 + 4 + tw) / 2;
    int ix = gx + 7;
    uint16_t orange = spr->color565(255, 140, 40);
    uint16_t yellow = spr->color565(255, 210, 60);
    spr->fillCircle(ix, cy + 2, 6, orange);
    spr->fillTriangle(ix - 6, cy + 2, ix, cy - 8, ix + 6, cy + 2, orange);
    spr->fillCircle(ix, cy + 3, 3, yellow);
    spr->drawString(buf, gx + 16 + 4, cy);
  }

  // --- points: sparkle ---
  fmtVal(statsPoints, buf, sizeof(buf));
  {
    int tw = spr->textWidth(buf);
    int gx = cols[1] - (16 + 4 + tw) / 2;
    int ix = gx + 7;
    uint16_t goldc = spr->color565(255, 200, 40);
    spr->fillTriangle(ix, cy - 7, ix - 3, cy, ix + 3, cy, goldc);
    spr->fillTriangle(ix, cy + 7, ix - 3, cy, ix + 3, cy, goldc);
    spr->fillTriangle(ix - 7, cy, ix, cy - 3, ix, cy + 3, goldc);
    spr->fillTriangle(ix + 7, cy, ix, cy - 3, ix, cy + 3, goldc);
    spr->drawString(buf, gx + 16 + 4, cy);
  }

  // --- happiness: heart ---
  {
    snprintf(buf, sizeof(buf), "%u", statsHappy);
    int tw = spr->textWidth(buf);
    int gx = cols[2] - (16 + 4 + tw) / 2;
    int ix = gx + 7;
    uint16_t pink = spr->color565(244, 63, 94);
    spr->fillCircle(ix - 3, cy - 2, 4, pink);
    spr->fillCircle(ix + 3, cy - 2, 4, pink);
    spr->fillTriangle(ix - 7, cy - 1, ix, cy + 7, ix + 7, cy - 1, pink);
    spr->drawString(buf, gx + 16 + 4, cy);
  }
}

// ---------------------------------------------------------------------------
// Top-right battery indicator: outline + nub, level fill, % label.
// ---------------------------------------------------------------------------

void FinagotchiPet::drawBattery() {
  if (!batteryKnown) return;

  const int bx = spr->width() - 30;   // body top-left
  const int by = 8;
  const int bw = 22, bh = 11;

  uint16_t frame = spr->color565(200, 210, 225);
  uint16_t bg = spr->color565(0x07, 0x11, 0x1F);

  spr->drawRect(bx, by, bw, bh, frame);
  spr->fillRect(bx + bw, by + 3, 2, bh - 6, frame);   // nub

  // Level fill: green > 60%, amber 25-60%, red below.
  uint16_t fill = spr->color565(244, 63, 94);
  if (batteryPct > 60)      fill = spr->color565(52, 211, 153);
  else if (batteryPct > 25) fill = spr->color565(251, 191, 36);
  int fw = (bw - 4) * batteryPct / 100;
  if (fw > 0) spr->fillRect(bx + 2, by + 2, fw, bh - 4, fill);

  char buf[5];
  snprintf(buf, sizeof(buf), "%u", batteryPct);
  spr->setTextDatum(MR_DATUM);
  spr->setTextSize(1);
  spr->setTextColor(frame, bg);
  spr->drawString(buf, bx - 3, by + bh / 2);
}

// ---------------------------------------------------------------------------
// Waiting-for-sync scene: two comets orbiting the pet with fading tails over
// a breathing orbit ring, plus a caption. Pure function of time (3 s
// revolution). Tails fade by blending cyan into the navy bg, like the glow.
// ---------------------------------------------------------------------------

void FinagotchiPet::drawSyncWait(float nowSec, float cx, float cy) {
  const float PERIOD = 3.0f;    // seconds per revolution
  const int   TAIL = 10;        // dots per comet tail
  float ro = R * 0.78f;         // orbit radius (clears body + reactions, and
                                // stays above the stats-bar divider)

  // Breathing orbit ring.
  float pulse = 0.5f + 0.5f * sinf(nowSec / 1.8f * TWO_PI);
  uint16_t ring = spr->color565(
      static_cast<uint8_t>(30 + 20 * pulse), static_cast<uint8_t>(40 + 26 * pulse),
      static_cast<uint8_t>(58 + 34 * pulse));
  spr->drawCircle(static_cast<int>(cx), static_cast<int>(cy), static_cast<int>(ro), ring);

  // Two comets, half a revolution apart, each with a fading dotted tail.
  for (int c = 0; c < 2; c++) {
    float head = (nowSec / PERIOD + static_cast<float>(c) * 0.5f) * TWO_PI;
    for (int j = 0; j < TAIL; j++) {
      float k = static_cast<float>(j) / static_cast<float>(TAIL - 1);   // 0 head -> 1 tail end
      float a = head - k * 1.1f;                // ~63 deg tail span
      float fade = (1.0f - k) * (1.0f - k);
      uint16_t col = spr->color565(
          static_cast<uint8_t>(0x22 * fade + 0x07 * (1.0f - fade)),
          static_cast<uint8_t>(0xD3 * fade + 0x11 * (1.0f - fade)),
          static_cast<uint8_t>(0xEE * fade + 0x1F * (1.0f - fade)));
      int r = (j == 0) ? 3 : (k < 0.4f ? 2 : 1);
      spr->fillCircle(static_cast<int>(cx + cosf(a) * ro), static_cast<int>(cy + sinf(a) * ro),
                      r, col);
    }
  }

  // Caption at the bottom: title with cycling ellipsis + dim subtitle.
  static const char* DOTS[4] = { "", ".", "..", "..." };
  char buf[28];
  snprintf(buf, sizeof(buf), "waiting for connection%s",
           DOTS[static_cast<int>(nowSec * 1.4f) & 3]);
  uint16_t bg = spr->color565(0x07, 0x11, 0x1F);
  int h = spr->height();
  spr->setTextDatum(TC_DATUM);
  spr->setTextSize(1);
  spr->setTextColor(spr->color565(200, 210, 225), bg);
  spr->drawString(buf, spr->width() / 2, h - 22);
  spr->setTextColor(spr->color565(80, 100, 122), bg);
  spr->drawString("open the Finagotchi app", spr->width() / 2, h - 10);
}

// ---------------------------------------------------------------------------

FinagotchiPet::Pose FinagotchiPet::poseFor(PetState id) const {
  const size_t idx = static_cast<size_t>(id);
  const StageDef& d = DEFS[idx];
  Pose p;
  memcpy(p.radii, gRadii[idx], sizeof(p.radii));
  p.offX = d.offX; p.offY = d.offY;
  p.gazeYaw = d.gazeYaw; p.gazePitch = d.gazePitch; p.gazeRoll = d.gazeRoll;
  p.split = d.split;
  for (int e = 0; e < 2; e++) {
    p.eyes[e] = { d.eyes[e].w, d.eyes[e].h, d.eyes[e].tilt, d.eyes[e].open };
  }
  p.eyeAlpha = 1.0f;
  p.bodyAlpha = 1.0f;
  p.fr = d.fr; p.fg = d.fg; p.fb = d.fb;
  p.gr = d.gr; p.gg = d.gg; p.gb = d.gb;
  p.hasGlow = d.hasGlow;
  return p;
}

// Pose at time t: state morph (same-angle radii lerp, shape.ts blend), then
// the expression override (expressions.ts blendExpression over 0.45 s).
FinagotchiPet::Pose FinagotchiPet::poseAt(float nowSec) const {
  Pose to = poseFor(cur);
  Pose p = to;

  float morph = DEFS[static_cast<size_t>(cur)].morph;
  float k = (nowSec - tCur) / morph;
  if (k < 1.0f && hasFrom) {
    float t = easeOutQuint(clampf(k));
    for (int i = 0; i < NRAD; i++) p.radii[i] = lerpf(fromPose.radii[i], to.radii[i], t);
    p.offX = lerpf(fromPose.offX, to.offX, t);
    p.offY = lerpf(fromPose.offY, to.offY, t);
    p.fr = lerpf(fromPose.fr, to.fr, t);
    p.fg = lerpf(fromPose.fg, to.fg, t);
    p.fb = lerpf(fromPose.fb, to.fb, t);
    p.gr = lerpf(fromPose.gr, to.gr, t);
    p.gg = lerpf(fromPose.gg, to.gg, t);
    p.gb = lerpf(fromPose.gb, to.gb, t);
    // blendPose: glowColor = t < 0.5 ? a : b (no glow until mid-morph)
    p.hasGlow = t < 0.5f ? fromPose.hasGlow : to.hasGlow;
  }

  // Expression override (engine.ts posed(): expr replaces gaze/split/eyes).
  const ExprDef& ea = EXPR[static_cast<size_t>(prevMood)];
  const ExprDef& eb = EXPR[static_cast<size_t>(curMood)];
  float me = easeOutQuint(clampf((nowSec - moodAtT) / 0.45f));
  p.gazeYaw = lerpf(ea.gazeYaw, eb.gazeYaw, me);
  p.gazePitch = lerpf(ea.gazePitch, eb.gazePitch, me);
  p.gazeRoll = lerpf(ea.gazeRoll, eb.gazeRoll, me);
  p.split = lerpf(ea.split, eb.split, me);
  for (int e = 0; e < 2; e++) {
    p.eyes[e].w = lerpf(ea.eyes[e].w, eb.eyes[e].w, me);
    p.eyes[e].h = lerpf(ea.eyes[e].h, eb.eyes[e].h, me);
    p.eyes[e].tilt = lerpf(ea.eyes[e].tilt, eb.eyes[e].tilt, me);
    p.eyes[e].open = lerpf(ea.eyes[e].open, eb.eyes[e].open, me);
  }
  return p;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

// unit (ball-radius) coords -> screen, with reaction rotation/scale.
void FinagotchiPet::mapPoint(float ux, float uy, float rotC, float rotS,
                             float scale, float cx, float cy,
                             float& sx, float& sy) const {
  float x = ux * R * scale;
  float y = uy * R * scale;
  sx = cx + x * rotC - y * rotS;
  sy = cy + x * rotS + y * rotC;
}

// Fill the body polygon (dx/dy buffers) as a triangle fan from its centroid.
void FinagotchiPet::fillPoly(uint16_t color) {
  float mx = 0, my = 0;
  for (int i = 0; i < NRAD; i++) { mx += dx[i]; my += dy[i]; }
  int icx = static_cast<int>(mx / NRAD), icy = static_cast<int>(my / NRAD);
  for (int i = 0; i < NRAD; i++) {
    int j = (i + 1) % NRAD;
    spr->fillTriangle(icx, icy, static_cast<int>(dx[i]), static_cast<int>(dy[i]),
                      static_cast<int>(dx[j]), static_cast<int>(dy[j]), color);
  }
}

// Sphere-projected eye (face.ts eyePoses + engine.ts composeFrame matrix),
// blink squish, alpha approximated by blending toward the body color.
void FinagotchiPet::drawEye(const Pose& p, const EyePose& e, const EyeCfg& cfg,
                            float lid, float rotC, float rotS, float scale,
                            float cx, float cy, uint16_t bodyColor) {
  if (e.depth <= 0.02f) return;

  float fit = radiusAt(p.radii, atan2f(e.y, e.x));   // sil.rot = 0
  float phi = cfg.tilt * DEG_TO_RAD;
  float cp = cosf(phi), sp = sinf(phi);
  float ax = e.a * cp + e.c * sp;
  float ay = e.b * cp + e.d * sp;
  float cxx = -e.a * sp + e.c * cp;
  float cyy = -e.b * sp + e.d * cp;
  float k = blinkScale(fminf(lid, cfg.open));

  float alpha = clampf(p.eyeAlpha * e.depth / 0.12f);
  // PetEyes fill is #f5f5f5; blend toward the body for the alpha fade.
  float er = 0xF5 * alpha + p.fr * (1.0f - alpha);
  float eg = 0xF5 * alpha + p.fg * (1.0f - alpha);
  float eb = 0xF5 * alpha + p.fb * (1.0f - alpha);
  uint16_t col = spr->color565(static_cast<uint8_t>(er), static_cast<uint8_t>(eg), static_cast<uint8_t>(eb));

  float tx = cx + e.x * fit;
  float ty = cy + e.y * fit;

  float lx[20], ly[20];
  int n = capsulePoints(cfg.w * R * scale, cfg.h * R * scale, lx, ly, 20);

  int icx = static_cast<int>(cx + (tx - cx) * rotC - (ty - cy) * rotS);
  int icy = static_cast<int>(cy + (tx - cx) * rotS + (ty - cy) * rotC);

  float exs[20], eys[20];
  for (int i = 0; i < n; i++) {
    float X = ax * lx[i] + cxx * ly[i] + tx;
    float Y = ay * k * lx[i] + cyy * k * ly[i] + ty;
    exs[i] = cx + (X - cx) * rotC - (Y - cy) * rotS;
    eys[i] = cy + (X - cx) * rotS + (Y - cy) * rotC;
  }
  for (int i = 0; i < n; i++) {
    int j = (i + 1) % n;
    spr->fillTriangle(icx, icy, static_cast<int>(exs[i]), static_cast<int>(eys[i]),
                      static_cast<int>(exs[j]), static_cast<int>(eys[j]), col);
  }
}

// Fill an arbitrary polygon as a triangle fan from its centroid.
void FinagotchiPet::fillPolyN(const float* xs, const float* ys, int n,
                              uint16_t color) {
  float mx = 0, my = 0;
  for (int i = 0; i < n; i++) { mx += xs[i]; my += ys[i]; }
  int icx = static_cast<int>(mx / n), icy = static_cast<int>(my / n);
  for (int i = 0; i < n; i++) {
    int j = (i + 1) % n;
    spr->fillTriangle(icx, icy, static_cast<int>(xs[i]), static_cast<int>(ys[i]),
                      static_cast<int>(xs[j]), static_cast<int>(ys[j]), color);
  }
}

// ---------------------------------------------------------------------------
// Accessory anchors (engine.ts composeAnchors/composeShirt).
// Artwork (PetAccessory.tsx) is drawn around the origin in units of R and
// mapped through a per-frame anchor matrix derived from the same pose math
// that places the eyes, so cosmetics track posture, gaze and breath instead
// of floating at fixed canvas fractions.
// ---------------------------------------------------------------------------

namespace {

constexpr float REF_RADIUS = 0.5f;          // artwork reference body radius
constexpr float GLASSES_REST_HALF_SEP = 0.22f;  // lens centers rest at ±0.22R
constexpr float CHEEK_DIR_X = 0.804f, CHEEK_DIR_Y = -0.595f;

// Fitted tee outline as (screen-deg, scale-toward-center) pairs; y-down,
// 90° = bottom. Hem at 0.95 leaves a body-colored hem line; sleeves at 1.16.
constexpr float SHIRT_OUTLINE[15][2] = {
  {20, 0.90f}, {25, 1.16f}, {37, 1.16f}, {45, 0.93f},     // collar, sleeve, underarm (right)
  {54, 0.95f}, {66, 0.95f}, {78, 0.95f}, {90, 0.95f},     // hem follows the silhouette
  {102, 0.95f}, {114, 0.95f}, {126, 0.95f},
  {135, 0.93f}, {143, 1.16f}, {155, 1.16f}, {160, 0.90f}, // underarm, sleeve, collar (left)
};
constexpr float SHIRT_COLLAR[2][2] = { {24, 0.87f}, {156, 0.87f} };

// c1 blended toward c2 by t — the app's rgba()/opacity artwork, flattened.
uint16_t blend565(TFT_eSprite* spr, uint8_t r1, uint8_t g1, uint8_t b1,
                  uint8_t r2, uint8_t g2, uint8_t b2, float t) {
  return spr->color565(
      static_cast<uint8_t>(r1 + (r2 - r1) * t),
      static_cast<uint8_t>(g1 + (g2 - g1) * t),
      static_cast<uint8_t>(b1 + (b2 - b1) * t));
}

} // namespace

void FinagotchiPet::anchorPoint(const Anchor& an, float ux, float uy,
                                float& sx, float& sy) const {
  sx = an.e + (an.a * ux + an.c * uy) * R;
  sy = an.f + (an.b * ux + an.d * uy) * R;
}

// Anchor matrix for the current item. eyeX/eyeY are the live eye centers
// (screen space); eyesOk = both eyes rendered this frame. Returns false when
// the anchor doesn't exist (glasses with no visible eyes).
bool FinagotchiPet::anchorFor(const Pose& p, const float eyeX[2],
                              const float eyeY[2], bool eyesOk,
                              float gazeRoll, float breath,
                              float rotC, float rotS, float rRotDeg,
                              float rScale, float cx, float cy,
                              Anchor& out) const {
  const float rRotRad = rRotDeg * DEG_TO_RAD;

  // Contour radius along a unit direction. The screen position re-applies
  // the reaction rotation via mapPoint, so subtract it here (fitAt).
  auto fitAt = [&](float dx, float dy) {
    float angle = fmodf(atan2f(dy, dx) - rRotRad, TWO_PI);
    if (angle < 0.0f) angle += TWO_PI;
    return radiusAt(p.radii, angle);
  };
  // Artwork scale for a direction: body size relative to the reference body.
  auto sizeAt = [&](float dx, float dy) {
    return clampf(fitAt(dx, dy) / REF_RADIUS, 0.7f, 1.4f);
  };
  // Screen point on the body contour (breath on y + reaction, like the body).
  auto contour = [&](float dx, float dy, float frac, float& sx, float& sy) {
    float fit = fitAt(dx, dy);
    mapPoint(dx * fit * frac, dy * fit * frac * breath, rotC, rotS, rScale,
             cx, cy, sx, sy);
  };
  auto matrix = [](float x, float y, float theta, float sx, float sy,
                   Anchor& an) {
    float c = cosf(theta), s = sinf(theta);
    an.a = sx * c; an.b = sx * s; an.c = -sy * s; an.d = sy * c;
    an.e = x;      an.f = y;
  };

  const float roll = (gazeRoll + rRotDeg) * DEG_TO_RAD;
  float x, y, s;

  switch (curItem) {
    case PetItem::ITEM_GLASSES: {   // face anchor
      if (!eyesOk) return false;
      float mx = (eyeX[0] + eyeX[1]) * 0.5f;
      float my = (eyeY[0] + eyeY[1]) * 0.5f;
      float tilt = atan2f(eyeY[1] - eyeY[0], eyeX[1] - eyeX[0]);
      float sep = hypotf(eyeX[1] - eyeX[0], eyeY[1] - eyeY[0]) * 0.5f;
      float sx = clampf(sep / (GLASSES_REST_HALF_SEP * R), 0.4f, 1.6f);
      matrix(mx, my, tilt, sx, 1.0f, out);   // lands the lenses on the eyes
      return true;
    }
    case PetItem::ITEM_CROWN:       // headTop
    case PetItem::ITEM_HALO:        // aboveHead
      contour(0.0f, -1.0f, 1.0f, x, y);
      s = breath * sizeAt(0.0f, -1.0f) * rScale;
      if (curItem == PetItem::ITEM_HALO) y -= 0.18f * R * s;
      matrix(x, y, roll, s, s, out);
      return true;
    case PetItem::ITEM_BOWTIE:      // chest
      contour(0.0f, 1.0f, 0.78f, x, y);
      s = breath * sizeAt(0.0f, 1.0f) * rScale;
      matrix(x, y, roll, s, s, out);
      return true;
    case PetItem::ITEM_TSHIRT:      // emblem rides the torso anchor
      contour(0.0f, 1.0f, 0.42f, x, y);
      s = breath * sizeAt(0.0f, 1.0f) * rScale;
      matrix(x, y, roll, s, s, out);
      return true;
    case PetItem::ITEM_DIAMOND:     // cheek
      contour(CHEEK_DIR_X, CHEEK_DIR_Y, 0.95f, x, y);
      s = breath * sizeAt(CHEEK_DIR_X, CHEEK_DIR_Y) * rScale;
      matrix(x, y, roll, s, s, out);
      return true;
    default:
      return false;
  }
}

// PetAccessory.tsx artwork, drawn around the origin (the anchor point) in
// units of R and mapped through the anchor matrix.
void FinagotchiPet::drawItem(const Anchor& an) {
  float xs[16], ys[16];
  auto fillArt = [&](const float* ux, const float* uy, int n, uint16_t color) {
    for (int i = 0; i < n; i++) anchorPoint(an, ux[i], uy[i], xs[i], ys[i]);
    fillPolyN(xs, ys, n, color);
  };
  // Ellipse as a 16-gon so anchor rotation/scale applies.
  auto fillEllipseArt = [&](float ecx, float ecy, float erx, float ery,
                            uint16_t color) {
    for (int i = 0; i < 16; i++) {
      float t = static_cast<float>(i) / 16.0f * TWO_PI;
      anchorPoint(an, ecx + erx * cosf(t), ecy + ery * sinf(t), xs[i], ys[i]);
    }
    fillPolyN(xs, ys, 16, color);
  };
  // ~2 px stroke between two artwork points.
  auto strokeArt = [&](float x0, float y0, float x1, float y1, uint16_t color) {
    float ax, ay, bx, by;
    anchorPoint(an, x0, y0, ax, ay);
    anchorPoint(an, x1, y1, bx, by);
    spr->drawLine(static_cast<int>(ax), static_cast<int>(ay),
                  static_cast<int>(bx), static_cast<int>(by), color);
    spr->drawLine(static_cast<int>(ax), static_cast<int>(ay) + 1,
                  static_cast<int>(bx), static_cast<int>(by) + 1, color);
  };

  const uint16_t gold = spr->color565(0xFB, 0xBF, 0x24);
  const uint16_t navy = spr->color565(0x07, 0x11, 0x1F);

  switch (curItem) {
    case PetItem::ITEM_CROWN: {
      // base y=0, notch -0.08R (at ±0.55·w), peak -0.20R, half-width 0.28R;
      // bottom edge is a shallow scoop approximated by its midpoint.
      const float ux[6] = { -0.28f, -0.154f, 0.0f, 0.154f, 0.28f, 0.0f };
      const float uy[6] = { 0.0f, -0.08f, -0.20f, -0.08f, 0.0f, 0.08f };
      fillArt(ux, uy, 6, gold);
      // inner accent: #d97706 at 35% over the crown fill
      uint16_t accent = blend565(spr.get(), 0xFB, 0xBF, 0x24, 0xD9, 0x77, 0x06, 0.35f);
      strokeArt(-0.12f, -0.12f, 0.0f, -0.04f, accent);
      strokeArt(0.0f, -0.04f, 0.12f, -0.12f, accent);
      break;
    }
    case PetItem::ITEM_GLASSES: {
      // rgba(31,41,55,0.82) over the navy scene
      uint16_t lens = blend565(spr.get(), 0x07, 0x11, 0x1F, 31, 41, 55, 0.82f);
      fillEllipseArt(-0.22f, 0.0f, 0.18f, 0.14f, lens);   // lens centers rest
      fillEllipseArt(0.22f, 0.0f, 0.18f, 0.14f, lens);    // at ±0.22R
      strokeArt(-0.04f, 0.0f, 0.04f, 0.0f, lens);         // bridge
      break;
    }
    case PetItem::ITEM_BOWTIE: {
      const float ux[6] = { 0.0f, -0.24f, -0.24f, 0.0f, 0.24f, 0.24f };
      const float uy[6] = { -0.06f, -0.14f, 0.14f, 0.06f, 0.14f, -0.14f };
      fillArt(ux, uy, 6, spr->color565(0xF4, 0x3F, 0x5E));
      strokeArt(0.0f, -0.04f, 0.0f, 0.04f, spr->color565(0xBE, 0x12, 0x3C));
      break;
    }
    case PetItem::ITEM_HALO: {
      // ring rx 0.34R / ry 0.07R, stroke 0.04R, gold at 92% over navy
      uint16_t ring = blend565(spr.get(), 0x07, 0x11, 0x1F, 0xFB, 0xBF, 0x24, 0.92f);
      fillEllipseArt(0.0f, 0.0f, 0.34f, 0.07f, ring);
      fillEllipseArt(0.0f, 0.0f, 0.30f, 0.03f, navy);
      break;
    }
    case PetItem::ITEM_DIAMOND: {
      const float ux[4] = { 0.0f, 0.16f, 0.0f, -0.16f };
      const float uy[4] = { -0.16f, 0.0f, 0.16f, 0.0f };
      fillArt(ux, uy, 4, spr->color565(0x22, 0xD3, 0xEE));
      // facets: #cffafe at 55% over the diamond fill
      uint16_t facet = blend565(spr.get(), 0x22, 0xD3, 0xEE, 0xCF, 0xFA, 0xFE, 0.55f);
      const float fx[3][3] = { {0.0f, -0.056f, 0.056f}, {-0.16f, -0.056f, 0.0f},
                               {0.16f, 0.056f, 0.0f} };
      const float fy[3][3] = { {-0.16f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.16f},
                               {0.0f, 0.0f, 0.16f} };
      for (int f = 0; f < 3; f++) fillArt(fx[f], fy[f], 3, facet);
      break;
    }
    case PetItem::ITEM_TSHIRT: {
      // Fitted tee: the engine-drawn shirt wraps the body; only the coin
      // emblem rides the torso anchor here.
      fillEllipseArt(0.0f, 0.12f, 0.06f, 0.06f, gold);
      float cxp, cyp, rxp, ryp;
      anchorPoint(an, 0.0f, 0.12f, cxp, cyp);
      anchorPoint(an, 0.033f, 0.12f, rxp, ryp);
      int r = static_cast<int>(hypotf(rxp - cxp, ryp - cyp));
      uint16_t dark = spr->color565(0xD9, 0x77, 0x06);
      spr->drawCircle(static_cast<int>(cxp), static_cast<int>(cyp), r, dark);
      if (r > 1)
        spr->drawCircle(static_cast<int>(cxp), static_cast<int>(cyp), r - 1, dark);
      break;
    }
    default: break;
  }
}

// Fitted tee (composeShirt): the outline is re-computed every frame from the
// live body contour points (dx/dy screen buffer), so it hugs every stage and
// breathes/drifts/reacts with the body.
void FinagotchiPet::drawShirt(float cx, float cy) {
  // Contour point at a screen angle, lerped between the two nearest samples.
  auto ptAt = [&](float deg, float& px, float& py) {
    float t = deg / 360.0f * NRAD;
    float tf = floorf(t);
    int i0 = (static_cast<int>(tf) % NRAD + NRAD) % NRAD;
    int i1 = (i0 + 1) % NRAD;
    float k = t - tf;
    px = lerpf(dx[i0], dx[i1], k);
    py = lerpf(dy[i0], dy[i1], k);
  };
  // Contour point scaled toward (k < 1) or past (k > 1) the body center.
  auto at = [&](float deg, float k, float& px, float& py) {
    float bx, by;
    ptAt(deg, bx, by);
    px = cx + (bx - cx) * k;
    py = cy + (by - cy) * k;
  };

  float xs[15], ys[15];
  for (int i = 0; i < 15; i++)
    at(SHIRT_OUTLINE[i][0], SHIRT_OUTLINE[i][1], xs[i], ys[i]);
  fillPolyN(xs, ys, 15, spr->color565(0xF1, 0xF5, 0xF9));

  // Collar trim: quadratic from at(24,0.87) to at(156,0.87), control pushed
  // down 0.06R, ~2 px with round caps, #94a3b8.
  float c1x, c1y, c2x, c2y;
  at(SHIRT_COLLAR[0][0], SHIRT_COLLAR[0][1], c1x, c1y);
  at(SHIRT_COLLAR[1][0], SHIRT_COLLAR[1][1], c2x, c2y);
  float qx = (c1x + c2x) * 0.5f;
  float qy = fmaxf(c1y, c2y) + 0.06f * R;
  uint16_t trim = spr->color565(0x94, 0xA3, 0xB8);
  const int SEG = 12;
  float px = c1x, py = c1y;
  for (int i = 1; i <= SEG; i++) {
    float t = static_cast<float>(i) / SEG;
    float u = 1.0f - t;
    float x = u * u * c1x + 2.0f * u * t * qx + t * t * c2x;
    float y = u * u * c1y + 2.0f * u * t * qy + t * t * c2y;
    spr->drawLine(static_cast<int>(px), static_cast<int>(py),
                  static_cast<int>(x), static_cast<int>(y), trim);
    spr->drawLine(static_cast<int>(px), static_cast<int>(py) + 1,
                  static_cast<int>(x), static_cast<int>(y) + 1, trim);
    px = x; py = y;
  }
  spr->fillCircle(static_cast<int>(c1x), static_cast<int>(c1y), 1, trim);
  spr->fillCircle(static_cast<int>(c2x), static_cast<int>(c2y), 1, trim);
}

// LevelUpAnimation: sparkle burst on stage-up (700 ms, rises and fades).
void FinagotchiPet::drawBurst(float nowSec, float cx, float cy) {
  float age = nowSec - burstT;
  if (age < 0.0f || age > 0.7f) return;
  float k = age / 0.7f;
  float fade = 1.0f - k;
  uint16_t col = spr->color565(static_cast<uint8_t>(0x8B * fade), static_cast<uint8_t>(0x5C * fade),
                               static_cast<uint8_t>(0xF6 * fade));
  float rise = -14.0f * (R / 75.0f) * k;
  float rot = 25.0f * k;
  float dist = R * (0.9f + 0.6f * easeOutCubic(k));
  int s = static_cast<int>(fmaxf(1.0f, 3.0f * fade * (R / 105.0f)));
  for (int i = 0; i < 6; i++) {
    float a = (60.0f * i + rot) * DEG_TO_RAD;
    int sx = static_cast<int>(cx + cosf(a) * dist);
    int sy = static_cast<int>(cy + sinf(a) * dist + rise);
    spr->drawLine(sx - s, sy, sx + s, sy, col);
    spr->drawLine(sx, sy - s, sx, sy + s, col);
  }
}

// ---------------------------------------------------------------------------

void FinagotchiPet::render(float nowSec) {
  if (!spr || !spr->created()) return;

  Pose pose = poseAt(nowSec);

  // look morph (engine.ts lookAtTime, 0.24 s)
  float lk = clampf((nowSec - lookAtT) / 0.24f);
  float le = easeOutQuint(lk);
  float lkYaw = lerpf(lookPrevYaw, lookYaw, le);
  float lkPitch = lerpf(lookPrevPitch, lookPitch, le);
  float lkMix = lerpf(lookPrevMix, lookMix, le);
  float lkWander = lerpf(lookPrevWander, lookWander, le);

  // liveliness (face.ts, exact)
  bool alive = pose.eyeAlpha > 0.01f;
  float wander = alive ? lkWander : 0.0f;
  float dYaw = (loopNoise(nowSec, 11.3f, 0.4f) * 5.5f +
                loopNoise(nowSec, 3.7f, 2.1f) * 1.6f) * wander;
  float dPitch = (loopNoise(nowSec, 9.1f, 1.3f) * 4.2f +
                  loopNoise(nowSec, 4.3f, 0.7f) * 1.3f) * wander;
  float dRoll = loopNoise(nowSec, 13.7f, 3.2f) * 2.2f * wander;
  float driftX = loopNoise(nowSec, 7.9f, 1.9f) * 0.006f;
  float driftY = loopNoise(nowSec, 5.3f, 0.3f) * 0.007f;
  float breath = 1.0f + sinf(nowSec / 3.4f * TWO_PI) * 0.005f;
  float lid = blinkScale(blinkLid(nowSec));   // engine applies blinkScale twice

  float gazeYaw = lerpf(pose.gazeYaw, lkYaw, lkMix) + dYaw;
  float gazePitch = lerpf(pose.gazePitch, lkPitch, lkMix) + dPitch;
  float gazeRoll = pose.gazeRoll + dRoll;

  // reaction keyframes (PetCanvas)
  float rScale = 1.0f, rRot = 0.0f;
  if (reaction != PetReaction::REACT_NONE) {
    const ReactKey* keys = KEYS_JUMP.data();
    size_t nk = KEYS_JUMP.size();
    switch (reaction) {
      case PetReaction::REACT_JUMP:  keys = KEYS_JUMP.data();  nk = KEYS_JUMP.size(); break;
      case PetReaction::REACT_SPIN:  keys = KEYS_SPIN.data();  nk = KEYS_SPIN.size(); break;
      case PetReaction::REACT_GLOW:  keys = KEYS_GLOW.data();  nk = KEYS_GLOW.size(); break;
      case PetReaction::REACT_DANCE: keys = KEYS_DANCE.data(); nk = KEYS_DANCE.size(); break;
      default: break;
    }
    float age = nowSec - reactT;
    if (age >= keys[nk - 1].t) {
      reaction = PetReaction::REACT_NONE;
    } else if (age > 0.0f) {
      size_t seg = 0;
      while (seg < nk - 2 && keys[seg + 1].t < age) seg++;
      float f = easeOutCubic(clampf((age - keys[seg].t) / (keys[seg + 1].t - keys[seg].t)));
      rScale = lerpf(keys[seg].s, keys[seg + 1].s, f);
      rRot = lerpf(keys[seg].r, keys[seg + 1].r, f);
    }
  }

  // idle float (PetCanvas worklet): 4 s loop, ±4 px at R=75
  float idleY = sinf(fmodf(nowSec, 4.0f) / 4.0f * TWO_PI * 0.45f) * -4.0f * (R / 75.0f);

  // Pet sits slightly above center to leave room for the bottom stats bar.
  float cx = spr->width() * 0.5f + (pose.offX + driftX) * R;
  float cy = spr->height() * 0.44f + (pose.offY + driftY) * R + idleY;
  float rotC = cosf(rRot * DEG_TO_RAD), rotS = sinf(rRot * DEG_TO_RAD);

  // App background is deep navy #07111F (PetCanvas).
  uint16_t bgColor = spr->color565(0x07, 0x11, 0x1F);
  uint16_t bodyColor = spr->color565(static_cast<uint8_t>(pose.fr), static_cast<uint8_t>(pose.fg), static_cast<uint8_t>(pose.fb));

  spr->fillSprite(bgColor);

  if (pose.hasGlow) {
    // PetBody glow: same path x1.15 behind the body at 22% opacity,
    // blended over the navy background.
    uint16_t glowDim = spr->color565(
        static_cast<uint8_t>(pose.gr * 0.22f + 0x07 * 0.78f),
        static_cast<uint8_t>(pose.gg * 0.22f + 0x11 * 0.78f),
        static_cast<uint8_t>(pose.gb * 0.22f + 0x1F * 0.78f));
    for (int i = 0; i < NRAD; i++) {
      float theta = static_cast<float>(i) * (TWO_PI / static_cast<float>(NRAD));
      float r = pose.radii[i];
      mapPoint(cosf(theta) * r, sinf(theta) * r * breath,
               rotC, rotS, rScale * 1.15f, cx, cy, dx[i], dy[i]);
    }
    fillPoly(glowDim);
  }

  for (int i = 0; i < NRAD; i++) {
    float theta = static_cast<float>(i) * (TWO_PI / static_cast<float>(NRAD));
    float r = pose.radii[i];
    mapPoint(cosf(theta) * r, sinf(theta) * r * breath,
             rotC, rotS, rScale, cx, cy, dx[i], dy[i]);
  }
  fillPoly(bodyColor);

  // Fitted tee wraps the live body contour; drawn before the eyes.
  // Z-order: body -> shirt -> collar -> eyes -> anchored accessory.
  if (curItem == PetItem::ITEM_TSHIRT) drawShirt(cx, cy);

  float eyeX[2] = {0.0f, 0.0f}, eyeY[2] = {0.0f, 0.0f};
  bool eyesOk = false;
  if (alive) {
    EyePose ep[2];
    eyePoses(gazeYaw, gazePitch, gazeRoll, R, pose.split, ep);
    eyesOk = ep[0].depth > 0.02f && ep[1].depth > 0.02f;
    for (int i = 0; i < 2; i++) {
      // Same screen position math as drawEye — the face anchor rides these.
      float fit = radiusAt(pose.radii, atan2f(ep[i].y, ep[i].x));
      float tx = cx + ep[i].x * fit, ty = cy + ep[i].y * fit;
      eyeX[i] = cx + (tx - cx) * rotC - (ty - cy) * rotS;
      eyeY[i] = cy + (tx - cx) * rotS + (ty - cy) * rotC;
      drawEye(pose, ep[i], pose.eyes[i], lid, rotC, rotS, rScale,
              cx, cy, bodyColor);
    }
  }

  if (curItem != PetItem::ITEM_NONE) {
    Anchor an;
    if (anchorFor(pose, eyeX, eyeY, eyesOk, gazeRoll, breath,
                  rotC, rotS, rRot, rScale, cx, cy, an)) {
      drawItem(an);
    }
  }
  drawBurst(nowSec, cx, cy);
  // Stats bar (streak / points / happiness) is ALWAYS on screen. While the
  // device advertises for the app, the orbiting-comets scene plays around
  // the pet and its caption lines sit below the bar. The bar is drawn last
  // so it wins any stray overlap with a comet dipping low.
  if (syncWait) drawSyncWait(nowSec, cx, cy);
  drawStatsBar();
  drawBattery();

  int sx = (tft->width() - spr->width()) / 2;
  int sy = (tft->height() - spr->height()) / 2;
  spr->pushSprite(sx, sy);
}
