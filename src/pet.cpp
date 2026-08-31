#include "pet.h"

// ---------------------------------------------------------------------------
// utils/math.ts
// ---------------------------------------------------------------------------

static inline float clampf(float v, float lo = 0.0f, float hi = 1.0f) {
  return v < lo ? lo : (v > hi ? hi : v);
}
static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
static inline float easeOutQuint(float t) {
  float u = 1.0f - t;
  return 1.0f - u * u * u * u * u;
}
static inline float easeOutCubic(float t) {
  float u = 1.0f - t;
  return 1.0f - u * u * u;
}

static float loopNoise(float t, float period, float seed) {
  float p = (t / period) * TWO_PI;
  return 0.55f * sinf(p + seed)
       + 0.30f * sinf(2.0f * p + seed * 1.7f + 1.1f)
       + 0.15f * sinf(3.0f * p + seed * 2.3f + 2.4f);
}

// createRng — mulberry32-style, bit-exact port
static uint32_t rngState;
static float rngNext() {
  rngState += 0x6D2B79F5u;
  uint32_t t = rngState;
  t = (t ^ (t >> 15)) * (1u | t);
  uint32_t prod = (t ^ (t >> 7)) * (61u | t);
  t = (t + prod) ^ t;
  return (float)(t ^ (t >> 14)) / 4294967296.0f;
}

// ---------------------------------------------------------------------------
// profiles.ts — exact radial profiles, 64 samples.
// theta = 0 points right, grows clockwise (y-down screen coords).
// GHOST_PROFILES.hem/.curl/.arms are copied verbatim from profiles.ts
// (generated from the ghost IP artwork by tools/radial_convert.py, max radius
// normalized to 0.55). Do not hand-edit; regenerate from the app repo.
// ---------------------------------------------------------------------------

static const float GHOST_HEM[FinagotchiPet::NRAD] = {
  0.3912f, 0.3923f, 0.3965f, 0.4039f, 0.4144f, 0.4281f, 0.4451f, 0.4662f,
  0.4900f, 0.5104f, 0.5190f, 0.5092f, 0.4776f, 0.4559f, 0.4468f, 0.4538f,
  0.4466f, 0.4399f, 0.4479f, 0.4681f, 0.4849f, 0.4837f, 0.4959f, 0.5184f,
  0.5477f, 0.5500f, 0.5180f, 0.4687f, 0.4199f, 0.3910f, 0.3763f, 0.3691f,
  0.3670f, 0.3687f, 0.3733f, 0.3809f, 0.3914f, 0.4045f, 0.4186f, 0.4329f,
  0.4464f, 0.4592f, 0.4708f, 0.4816f, 0.4910f, 0.4988f, 0.5047f, 0.5087f,
  0.5113f, 0.5121f, 0.5110f, 0.5079f, 0.5033f, 0.4965f, 0.4881f, 0.4778f,
  0.4666f, 0.4546f, 0.4418f, 0.4283f, 0.4157f, 0.4051f, 0.3978f, 0.3931f,
};

static const float GHOST_CURL[FinagotchiPet::NRAD] = {
  0.4336f, 0.4319f, 0.4317f, 0.4331f, 0.4359f, 0.4401f, 0.4447f, 0.4495f,
  0.4544f, 0.4586f, 0.4619f, 0.4640f, 0.4657f, 0.4668f, 0.4680f, 0.4693f,
  0.4718f, 0.4758f, 0.4819f, 0.4899f, 0.4998f, 0.5109f, 0.5231f, 0.5342f,
  0.5439f, 0.5500f, 0.5477f, 0.4891f, 0.4302f, 0.3783f, 0.3819f, 0.3878f,
  0.3947f, 0.4031f, 0.4117f, 0.4203f, 0.4289f, 0.4376f, 0.4460f, 0.4535f,
  0.4609f, 0.4678f, 0.4743f, 0.4798f, 0.4844f, 0.4882f, 0.4914f, 0.4937f,
  0.4956f, 0.4962f, 0.4966f, 0.4962f, 0.4951f, 0.4926f, 0.4895f, 0.4853f,
  0.4800f, 0.4739f, 0.4672f, 0.4605f, 0.4531f, 0.4464f, 0.4407f, 0.4365f,
};

static const float GHOST_ARMS[FinagotchiPet::NRAD] = {
  0.4486f, 0.4351f, 0.4082f, 0.3940f, 0.4023f, 0.4486f, 0.5026f, 0.5448f,
  0.5500f, 0.5282f, 0.4971f, 0.4683f, 0.4467f, 0.4310f, 0.4203f, 0.4141f,
  0.4122f, 0.4141f, 0.4203f, 0.4310f, 0.4467f, 0.4663f, 0.4834f, 0.4915f,
  0.4869f, 0.4731f, 0.4541f, 0.4342f, 0.4161f, 0.4045f, 0.4139f, 0.4384f,
  0.4659f, 0.4484f, 0.4240f, 0.4004f, 0.4106f, 0.4213f, 0.4327f, 0.4436f,
  0.4539f, 0.4628f, 0.4705f, 0.4764f, 0.4810f, 0.4836f, 0.4851f, 0.4851f,
  0.4845f, 0.4825f, 0.4797f, 0.4757f, 0.4709f, 0.4642f, 0.4561f, 0.4462f,
  0.4362f, 0.4248f, 0.4128f, 0.3999f, 0.3879f, 0.3772f, 0.3975f, 0.4244f,
};

static float gRadii[PET_STATE_COUNT][FinagotchiPet::NRAD];
static bool  gTablesReady = false;

static void buildTables() {
  if (gTablesReady) return;
  gTablesReady = true;

  for (int i = 0; i < FinagotchiPet::NRAD; i++) {
    float theta = (float)i / FinagotchiPet::NRAD * TWO_PI;
    float degrees = (float)i / FinagotchiPet::NRAD * 360.0f;
    float c = fabsf(cosf(theta));
    float s = fabsf(sinf(theta));

    // egg: superellipse, n 2.5 bottom / 1.8 top, wobble sin(3t) — unchanged
    float n = degrees < 180.0f ? 2.5f : 1.8f;
    float se = powf(powf(c, n) + powf(s, n), -1.0f / n);
    gRadii[PET_EGG][i] = clampf(0.45f * se + 0.015f * sinf(3.0f * theta));
  }

  // Ghost silhouettes ride on the coinling/hodler/whale stages (engine.ts).
  memcpy(gRadii[PET_COINLING], GHOST_HEM,  sizeof(GHOST_HEM));
  memcpy(gRadii[PET_HODLER],   GHOST_CURL, sizeof(GHOST_CURL));
  memcpy(gRadii[PET_WHALE],    GHOST_ARMS, sizeof(GHOST_ARMS));
}

// shape.ts radiusAtAngle: linear interpolation between nearest samples.
float FinagotchiPet::radiusAt(const float* radii, float theta) const {
  float t = theta * ((float)NRAD / TWO_PI);
  float tf = floorf(t);
  int i0 = ((int)tf % NRAD + NRAD) % NRAD;
  int i1 = (i0 + 1) % NRAD;
  return lerpf(radii[i0], radii[i1], t - tf);
}

// ---------------------------------------------------------------------------
// face.ts — blink schedule, blinkScale, eyePoses.
// ---------------------------------------------------------------------------

static float gBlinks[512];
static int   gBlinkCount = 0;
static const float BLINK_DUR = 0.18f;

static void buildBlinks() {
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

static float blinkLid(float t) {
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

static inline float blinkScale(float lid) { return 0.06f + 0.94f * clampf(lid); }

// Rotate two vectors in their common plane (face.ts spin).
static void spin3(const float u[3], const float v[3], float angle,
                  float outU[3], float outV[3]) {
  float c = cosf(angle), s = sinf(angle);
  for (int i = 0; i < 3; i++) {
    outU[i] = u[i] * c + v[i] * s;
    outV[i] = v[i] * c - u[i] * s;
  }
}

// face.ts eyePoses. Screen coords: x right, y down, z toward viewer.
// Index 0 = inner eye, index 1 = outer eye.
static void eyePoses(float yawDeg, float pitchDeg, float rollDeg,
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
static int capsulePoints(float w, float h, float* xs, float* ys, int maxN) {
  float r = fminf(w, h) * 0.5f;
  float L = fabsf(w - h) * 0.5f;
  bool horizontal = w >= h;
  const int H = 9;   // samples per semicircle
  int n = 0;
  for (int i = 0; i < H && n < maxN; i++) {
    float a = (-90.0f + 180.0f * (float)i / (float)(H - 1)) * DEG_TO_RAD;
    float px = L + r * cosf(a), py = r * sinf(a);
    xs[n] = horizontal ? px : py;
    ys[n] = horizontal ? py : px;
    n++;
  }
  for (int i = 0; i < H && n < maxN; i++) {
    float a = (90.0f + 180.0f * (float)i / (float)(H - 1)) * DEG_TO_RAD;
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

namespace {

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

const StageDef DEFS[PET_STATE_COUNT] = {
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

const ExprDef EXPR[MOOD_COUNT] = {
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
const ReactKey KEYS_JUMP[]  = {{0,1,0},{0.22f,1.28f,0},{0.45f,0.88f,0},{0.8f,1,0}};
const ReactKey KEYS_SPIN[]  = {{0,1,0},{0.12f,0.9f,0},{0.65f,1.08f,360},{0.9f,1,360}};
const ReactKey KEYS_GLOW[]  = {{0,1,0},{0.16f,1.16f,0},{0.34f,1.04f,0},{0.52f,1.14f,0},
                               {0.70f,1.04f,0},{0.88f,1.12f,0},{1.08f,1,0}};
const ReactKey KEYS_DANCE[] = {{0,1,0},{0.11f,1.06f,-14},{0.22f,0.94f,14},{0.33f,1.06f,-14},
                               {0.44f,0.94f,14},{0.55f,1.06f,-14},{0.66f,0.94f,14},
                               {0.77f,1.04f,-8},{0.95f,1,0}};

} // namespace

// ---------------------------------------------------------------------------
// FinagotchiPet
// ---------------------------------------------------------------------------

void FinagotchiPet::begin(TFT_eSPI* display, float scale) {
  tft = display;
  R = scale;
  buildTables();
  buildBlinks();
  spr = new TFT_eSprite(tft);
  spr->setColorDepth(16);
  if (spr->createSprite(tft->width(), tft->height()) == nullptr) {
    spr->createSprite(200, 200);   // RAM fallback
  }
}

void FinagotchiPet::end() {
  if (spr) { delete spr; spr = nullptr; }
}

void FinagotchiPet::setState(PetState id, float nowSec) {
  if (id == cur || id >= PET_STATE_COUNT) return;
  if (id > cur) burstT = nowSec;   // LevelUpAnimation trigger (stage up only)
  fromPose = poseAt(nowSec);       // departFige: morph from the visible pose
  hasFrom = true;
  cur = id;
  tCur = nowSec;
}

bool FinagotchiPet::evolve(float nowSec) {
  if (cur >= PET_WHALE) return false;
  setState((PetState)(cur + 1), nowSec);
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

void FinagotchiPet::setMood(uint8_t m, float nowSec) {
  if (m >= MOOD_COUNT || m == curMood) return;
  prevMood = curMood;
  curMood = m;
  moodAtT = nowSec;
}

void FinagotchiPet::setItem(uint8_t i) {
  if (i < ITEM_COUNT) curItem = i;
}

void FinagotchiPet::react(uint8_t r, float nowSec) {
  if (r > REACT_DANCE) return;
  reaction = r;
  reactT = nowSec;
}

void FinagotchiPet::setStats(uint32_t streakDays, uint32_t points,
                             uint8_t happiness) {
  statsStreak = streakDays;
  statsPoints = points;
  statsHappy = happiness > 100 ? 100 : happiness;
}

void FinagotchiPet::setSyncWait(bool on, float nowSec) {
  if (on == syncWait) return;
  syncWait = on;
  if (on) {
    preSyncMood = curMood;
    setMood(MOOD_WAITING, nowSec);
  } else {
    setMood(preSyncMood, nowSec);
  }
}

// ---------------------------------------------------------------------------
// Bottom stats bar: flame + streak, sparkle + points, heart + happiness.
// ---------------------------------------------------------------------------

static void fmtVal(uint32_t v, char* buf, size_t n) {
  if (v >= 100000)   snprintf(buf, n, "%luk", (unsigned long)(v / 1000));
  else if (v >= 10000) snprintf(buf, n, "%.1fk", v / 1000.0f);
  else               snprintf(buf, n, "%lu", (unsigned long)v);
}

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
// Waiting-for-sync scene: two comets orbiting the pet with fading tails over
// a breathing orbit ring, plus a caption. Pure function of time (3 s
// revolution). Tails fade by blending cyan into the navy bg, like the glow.
// ---------------------------------------------------------------------------

void FinagotchiPet::drawSyncWait(float nowSec, float cx, float cy) {
  const float PERIOD = 3.0f;    // seconds per revolution
  const int   TAIL = 10;        // dots per comet tail
  float ro = R * 0.82f;         // orbit radius (clears body + reactions)

  // Breathing orbit ring.
  float pulse = 0.5f + 0.5f * sinf(nowSec / 1.8f * TWO_PI);
  uint16_t ring = spr->color565(
      (uint8_t)(30 + 20 * pulse), (uint8_t)(40 + 26 * pulse),
      (uint8_t)(58 + 34 * pulse));
  spr->drawCircle((int)cx, (int)cy, (int)ro, ring);

  // Two comets, half a revolution apart, each with a fading dotted tail.
  for (int c = 0; c < 2; c++) {
    float head = (nowSec / PERIOD + (float)c * 0.5f) * TWO_PI;
    for (int j = 0; j < TAIL; j++) {
      float k = (float)j / (float)(TAIL - 1);   // 0 head -> 1 tail end
      float a = head - k * 1.1f;                // ~63 deg tail span
      float fade = (1.0f - k) * (1.0f - k);
      uint16_t col = spr->color565(
          (uint8_t)(0x22 * fade + 0x07 * (1.0f - fade)),
          (uint8_t)(0xD3 * fade + 0x11 * (1.0f - fade)),
          (uint8_t)(0xEE * fade + 0x1F * (1.0f - fade)));
      int r = (j == 0) ? 3 : (k < 0.4f ? 2 : 1);
      spr->fillCircle((int)(cx + cosf(a) * ro), (int)(cy + sinf(a) * ro),
                      r, col);
    }
  }

  // Caption at the bottom: title with cycling ellipsis + dim subtitle.
  static const char* DOTS[4] = { "", ".", "..", "..." };
  char buf[28];
  snprintf(buf, sizeof(buf), "waiting for connection%s",
           DOTS[(int)(nowSec * 1.4f) & 3]);
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
  const StageDef& d = DEFS[id];
  Pose p;
  memcpy(p.radii, gRadii[id], sizeof(p.radii));
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

  float morph = DEFS[cur].morph;
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
  const ExprDef& ea = EXPR[prevMood];
  const ExprDef& eb = EXPR[curMood];
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
  int icx = (int)(mx / NRAD), icy = (int)(my / NRAD);
  for (int i = 0; i < NRAD; i++) {
    int j = (i + 1) % NRAD;
    spr->fillTriangle(icx, icy, (int)dx[i], (int)dy[i],
                      (int)dx[j], (int)dy[j], color);
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
  uint16_t col = spr->color565((uint8_t)er, (uint8_t)eg, (uint8_t)eb);

  float tx = cx + e.x * fit;
  float ty = cy + e.y * fit;

  float lx[20], ly[20];
  int n = capsulePoints(cfg.w * R * scale, cfg.h * R * scale, lx, ly, 20);

  int icx = (int)(cx + (tx - cx) * rotC - (ty - cy) * rotS);
  int icy = (int)(cy + (tx - cx) * rotS + (ty - cy) * rotC);

  float exs[20], eys[20];
  for (int i = 0; i < n; i++) {
    float X = ax * lx[i] + cxx * ly[i] + tx;
    float Y = ay * k * lx[i] + cyy * k * ly[i] + ty;
    exs[i] = cx + (X - cx) * rotC - (Y - cy) * rotS;
    eys[i] = cy + (X - cx) * rotS + (Y - cy) * rotC;
  }
  for (int i = 0; i < n; i++) {
    int j = (i + 1) % n;
    spr->fillTriangle(icx, icy, (int)exs[i], (int)eys[i],
                      (int)exs[j], (int)eys[j], col);
  }
}

// PetAccessory.tsx collectibles. Positions are in ball-radius units relative
// to the pet center, exactly like the app's R-relative paths.
void FinagotchiPet::drawItem(float rotC, float rotS, float scale,
                             float cx, float cy) {
  auto mp = [&](float ux, float uy, float& sx, float& sy) {
    mapPoint(ux, uy, rotC, rotS, scale, cx, cy, sx, sy);
  };
  uint16_t gold = spr->color565(0xFB, 0xBF, 0x24);
  float x0, y0, x1, y1, x2, y2;

  switch (curItem) {
    case ITEM_CROWN: {
      // zigzag: (-.28,-.58)(-.154,-.66)(0,-.78)(.154,-.66)(.28,-.58)(0,-.50)
      const float ux[6] = { -0.28f, -0.154f, 0.0f, 0.154f, 0.28f, 0.0f };
      const float uy[6] = { -0.58f, -0.66f, -0.78f, -0.66f, -0.58f, -0.50f };
      float sx[6], sy[6];
      float mx = 0, my = 0;
      for (int i = 0; i < 6; i++) { mp(ux[i], uy[i], sx[i], sy[i]); mx += sx[i]; my += sy[i]; }
      int icx = (int)(mx / 6), icy = (int)(my / 6);
      for (int i = 0; i < 6; i++) {
        int j = (i + 1) % 6;
        spr->fillTriangle(icx, icy, (int)sx[i], (int)sy[i], (int)sx[j], (int)sy[j], gold);
      }
      break;
    }
    case ITEM_GLASSES: {
      uint16_t slate = spr->color565(0x1F, 0x29, 0x37);
      float lx, ly, rx, ry;
      mp(-0.22f, -0.10f, lx, ly);
      mp(0.22f, -0.10f, rx, ry);
      int erx = (int)(0.18f * R * scale), ery = (int)(0.14f * R * scale);
      spr->fillEllipse((int)lx, (int)ly, erx, ery, slate);
      spr->fillEllipse((int)rx, (int)ry, erx, ery, slate);
      float bx0, by0, bx1, by1;
      mp(-0.04f, -0.10f, bx0, by0);
      mp(0.04f, -0.10f, bx1, by1);
      spr->drawLine((int)bx0, (int)by0, (int)bx1, (int)by1, slate);
      spr->drawLine((int)bx0, (int)by0 + 1, (int)bx1, (int)by1 + 1, slate);
      break;
    }
    case ITEM_BOWTIE: {
      uint16_t red = spr->color565(0xF4, 0x3F, 0x5E);
      uint16_t dark = spr->color565(0xBE, 0x12, 0x3C);
      // (0,.42)(-.24,.34)(-.24,.62)(0,.54)(.24,.62)(.24,.34)
      const float ux[6] = { 0.0f, -0.24f, -0.24f, 0.0f, 0.24f, 0.24f };
      const float uy[6] = { 0.42f, 0.34f, 0.62f, 0.54f, 0.62f, 0.34f };
      float sx[6], sy[6];
      float kx, ky, kx1, ky1;
      mp(0.0f, 0.48f, kx, ky);
      for (int i = 0; i < 6; i++) mp(ux[i], uy[i], sx[i], sy[i]);
      for (int i = 0; i < 6; i++) {
        int j = (i + 1) % 6;
        spr->fillTriangle((int)kx, (int)ky, (int)sx[i], (int)sy[i],
                          (int)sx[j], (int)sy[j], red);
      }
      mp(0.0f, 0.44f, kx, ky);
      mp(0.0f, 0.52f, kx1, ky1);
      spr->drawLine((int)kx, (int)ky, (int)kx1, (int)ky1, dark);
      spr->drawLine((int)kx + 1, (int)ky, (int)kx1 + 1, (int)ky1, dark);
      break;
    }
    case ITEM_HALO: {
      float hx, hy;
      mp(0.0f, -0.82f, hx, hy);
      int rx = (int)(0.34f * R * scale), ry = (int)(0.07f * R * scale);
      spr->fillEllipse((int)hx, (int)hy, rx, ry, gold);
      spr->fillEllipse((int)hx, (int)hy, (int)(0.30f * R * scale),
                       (int)(0.03f * R * scale), spr->color565(0x07, 0x11, 0x1F));
      break;
    }
    case ITEM_DIAMOND: {
      uint16_t cyan = spr->color565(0x22, 0xD3, 0xEE);
      uint16_t light = spr->color565(0xCF, 0xFA, 0xFE);
      float dcx, dcy;
      mp(0.46f, -0.34f, dcx, dcy);
      float s = 0.16f * R * scale;
      spr->fillTriangle((int)dcx, (int)(dcy - s), (int)(dcx + s), (int)dcy,
                        (int)dcx, (int)(dcy + s), cyan);
      spr->fillTriangle((int)dcx, (int)(dcy - s), (int)(dcx - s), (int)dcy,
                        (int)dcx, (int)(dcy + s), cyan);
      spr->drawLine((int)(dcx - s), (int)dcy, (int)(dcx + s), (int)dcy, light);
      spr->drawLine((int)dcx, (int)(dcy - s), (int)dcx, (int)(dcy + s), light);
      break;
    }
    default: break;
  }
  (void)x0; (void)y0; (void)x1; (void)y1; (void)x2; (void)y2;
}

// LevelUpAnimation: sparkle burst on stage-up (700 ms, rises and fades).
void FinagotchiPet::drawBurst(float nowSec, float cx, float cy) {
  float age = nowSec - burstT;
  if (age < 0.0f || age > 0.7f) return;
  float k = age / 0.7f;
  float fade = 1.0f - k;
  uint16_t col = spr->color565((uint8_t)(0x8B * fade), (uint8_t)(0x5C * fade),
                               (uint8_t)(0xF6 * fade));
  float rise = -14.0f * (R / 75.0f) * k;
  float rot = 25.0f * k;
  float dist = R * (0.9f + 0.6f * easeOutCubic(k));
  int s = (int)fmaxf(1.0f, 3.0f * fade * (R / 105.0f));
  for (int i = 0; i < 6; i++) {
    float a = (60.0f * i + rot) * DEG_TO_RAD;
    int sx = (int)(cx + cosf(a) * dist);
    int sy = (int)(cy + sinf(a) * dist + rise);
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
  if (reaction != REACT_NONE) {
    const ReactKey* keys = KEYS_JUMP;
    int nk = sizeof(KEYS_JUMP) / sizeof(ReactKey);
    switch (reaction) {
      case REACT_JUMP:  keys = KEYS_JUMP;  nk = sizeof(KEYS_JUMP) / sizeof(ReactKey); break;
      case REACT_SPIN:  keys = KEYS_SPIN;  nk = sizeof(KEYS_SPIN) / sizeof(ReactKey); break;
      case REACT_GLOW:  keys = KEYS_GLOW;  nk = sizeof(KEYS_GLOW) / sizeof(ReactKey); break;
      case REACT_DANCE: keys = KEYS_DANCE; nk = sizeof(KEYS_DANCE) / sizeof(ReactKey); break;
      default: break;
    }
    float age = nowSec - reactT;
    if (age >= keys[nk - 1].t) {
      reaction = REACT_NONE;
    } else if (age > 0.0f) {
      int seg = 0;
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
  uint16_t bodyColor = spr->color565((uint8_t)pose.fr, (uint8_t)pose.fg, (uint8_t)pose.fb);

  spr->fillSprite(bgColor);

  if (pose.hasGlow) {
    // PetBody glow: same path x1.15 behind the body at 22% opacity,
    // blended over the navy background.
    uint16_t glowDim = spr->color565(
        (uint8_t)(pose.gr * 0.22f + 0x07 * 0.78f),
        (uint8_t)(pose.gg * 0.22f + 0x11 * 0.78f),
        (uint8_t)(pose.gb * 0.22f + 0x1F * 0.78f));
    for (int i = 0; i < NRAD; i++) {
      float theta = (float)i * (TWO_PI / (float)NRAD);
      float r = pose.radii[i];
      mapPoint(cosf(theta) * r, sinf(theta) * r * breath,
               rotC, rotS, rScale * 1.15f, cx, cy, dx[i], dy[i]);
    }
    fillPoly(glowDim);
  }

  for (int i = 0; i < NRAD; i++) {
    float theta = (float)i * (TWO_PI / (float)NRAD);
    float r = pose.radii[i];
    mapPoint(cosf(theta) * r, sinf(theta) * r * breath,
             rotC, rotS, rScale, cx, cy, dx[i], dy[i]);
  }
  fillPoly(bodyColor);

  if (alive) {
    EyePose ep[2];
    eyePoses(gazeYaw, gazePitch, gazeRoll, R, pose.split, ep);
    for (int i = 0; i < 2; i++) {
      drawEye(pose, ep[i], pose.eyes[i], lid, rotC, rotS, rScale,
              cx, cy, bodyColor);
    }
  }

  if (curItem != ITEM_NONE) drawItem(rotC, rotS, rScale, cx, cy);
  drawBurst(nowSec, cx, cy);
  // Stats are app-driven: only show the bar while the app is connected.
  // Disconnected shows the waiting-for-connection scene instead.
  if (syncWait) drawSyncWait(nowSec, cx, cy);
  else drawStatsBar();

  int sx = (tft->width() - spr->width()) / 2;
  int sy = (tft->height() - spr->height()) / 2;
  spr->pushSprite(sx, sy);
}
