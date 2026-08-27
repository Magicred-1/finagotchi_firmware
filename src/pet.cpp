#include "pet.h"

// ---------------------------------------------------------------------------
// Radial profiles — exact port of profiles.ts.
// 64 samples, theta = 0 points right, grows clockwise (y-down screen coords).
// ---------------------------------------------------------------------------

static float gRadii[PET_STATE_COUNT][FinagotchiPet::NRAD];
static bool  gTablesReady = false;

static inline float clampf(float v, float lo, float hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}
static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }

// easeOutQuint, same as the web engine
static inline float easeOutQuint(float t) {
  float u = 1.0f - t;
  return 1.0f - u * u * u * u * u;
}

static void buildTables() {
  if (gTablesReady) return;
  gTablesReady = true;

  for (int i = 0; i < FinagotchiPet::NRAD; i++) {
    float theta = (float)i / FinagotchiPet::NRAD * TWO_PI;
    float degrees = (float)i / FinagotchiPet::NRAD * 360.0f;
    float c = fabsf(cosf(theta));
    float s = fabsf(sinf(theta));

    // egg: superellipse, n = 2.5 bottom half / 1.8 top half, + wobble
    {
      float n = degrees < 180.0f ? 2.5f : 1.8f;
      float se = powf(powf(c, n) + powf(s, n), -1.0f / n);
      float wobble = 0.015f * sinf(3.0f * theta);
      gRadii[PET_EGG][i] = clampf(0.45f * se + wobble, 0.0f, 1.0f);
    }

    // coinling: n = 4 squircle + wobble
    {
      float sq = powf(powf(c, 4.0f) + powf(s, 4.0f), -0.25f);
      float wobble = 0.01f * sinf(5.0f * theta);
      gRadii[PET_COINLING][i] = clampf(0.48f * sq + wobble, 0.0f, 1.0f);
    }

    // hodler: (0.38 + 0.22|cos 2t|)^0.4
    {
      float base = 0.38f + 0.22f * fabsf(cosf(theta * 2.0f));
      gRadii[PET_HODLER][i] = clampf(powf(base, 0.4f), 0.0f, 1.0f);
    }

    // whale: ellipse a = 0.58, b = 0.36
    {
      const float a = 0.58f, b = 0.36f;
      float cw = cosf(theta), sw = sinf(theta);
      float den = sqrtf((b * cw) * (b * cw) + (a * sw) * (a * sw));
      gRadii[PET_WHALE][i] = clampf(a * b / (den > 0.0f ? den : 1.0f), 0.0f, 1.0f);
    }
  }
}

// ---------------------------------------------------------------------------
// State definitions — pose/eyes/colors straight from engine.ts STATE_DEFS.
// ---------------------------------------------------------------------------

namespace {

struct StateDef {
  float offX, offY;
  float gazeYaw, gazePitch;
  float split;
  float eyeW, eyeH, eyeTilt, eyeOpen;
  uint8_t r, g, b;
  bool    hasGlow;
  uint8_t gr, gg, gb;
};

const StateDef DEFS[PET_STATE_COUNT] = {
  // egg
  { 0.0f,  0.04f, 0.0f,  18.0f, 12.0f, 0.16f, 0.06f, 0.0f, 1.0f,
    0xD4, 0xC8, 0xB8, false, 0, 0, 0 },
  // coinling
  { 0.0f,  0.00f, 0.0f,  -8.0f, 16.0f, 0.22f, 0.22f, 0.0f, 1.0f,
    0xF5, 0x9E, 0x0B, true,  0xFB, 0xBF, 0x24 },
  // hodler
  { 0.0f, -0.03f, 0.0f,   2.0f, 14.0f, 0.22f, 0.09f, 0.0f, 1.0f,
    0x3B, 0x82, 0xF6, true,  0x60, 0xA5, 0xFA },
  // whale
  { 0.0f,  0.05f, 0.0f, -12.0f, 13.0f, 0.14f, 0.09f, 0.0f, 1.0f,
    0x63, 0x66, 0xF1, true,  0x81, 0x8C, 0xF8 },
};

} // namespace

// ---------------------------------------------------------------------------

void FinagotchiPet::begin(TFT_eSPI* display, float scale) {
  tft = display;
  R = scale;
  buildTables();
  spr = new TFT_eSprite(tft);
  spr->setColorDepth(16);
  if (spr->createSprite(tft->width(), tft->height()) == nullptr) {
    // Fall back to a smaller square around the body if RAM is tight.
    spr->createSprite(200, 200);
  }
}

void FinagotchiPet::end() {
  if (spr) { delete spr; spr = nullptr; }
}

void FinagotchiPet::setState(PetState id, float nowSec) {
  if (id == cur || id >= PET_STATE_COUNT) return;
  prev = cur;
  tPrev = tCur;
  cur = id;
  tCur = nowSec;
}

bool FinagotchiPet::evolve(float nowSec) {
  if (cur >= PET_WHALE) return false;
  setState((PetState)(cur + 1), nowSec);
  return true;
}

// radiusAtAngle from shape.ts: linear interpolation between nearest samples.
float FinagotchiPet::radiusAt(const float* radii, float theta) const {
  float t = theta * ((float)NRAD / TWO_PI);
  float tf = floorf(t);
  int i0 = ((int)tf % NRAD + NRAD) % NRAD;
  int i1 = (i0 + 1) % NRAD;
  return lerpf(radii[i0], radii[i1], t - tf);
}

FinagotchiPet::Pose FinagotchiPet::poseFor(PetState id) const {
  const StateDef& d = DEFS[id];
  Pose p;
  memcpy(p.radii, gRadii[id], sizeof(p.radii));
  p.offX = d.offX;       p.offY = d.offY;
  p.gazeYaw = d.gazeYaw; p.gazePitch = d.gazePitch;
  p.split = d.split;
  p.eyeW = d.eyeW; p.eyeH = d.eyeH; p.eyeTilt = d.eyeTilt; p.eyeOpen = d.eyeOpen;
  p.r = d.r; p.g = d.g; p.b = d.b;
  p.gr = d.gr; p.gg = d.gg; p.gb = d.gb;
  p.hasGlow = d.hasGlow;
  return p;
}

// Pose at time t, blending prev -> cur during the morph window (shape.ts blend:
// same-angle radii lerp, so elementwise interpolation is the exact morph).
FinagotchiPet::Pose FinagotchiPet::poseAt(float nowSec) const {
  Pose to = poseFor(cur);
  float k = (nowSec - tCur) / morphDur;
  if (k >= 1.0f || cur == prev) return to;

  Pose from = poseFor(prev);
  float t = easeOutQuint(clampf(k, 0.0f, 1.0f));
  Pose p;
  for (int i = 0; i < NRAD; i++) p.radii[i] = lerpf(from.radii[i], to.radii[i], t);
  p.offX = lerpf(from.offX, to.offX, t);
  p.offY = lerpf(from.offY, to.offY, t);
  p.gazeYaw = lerpf(from.gazeYaw, to.gazeYaw, t);
  p.gazePitch = lerpf(from.gazePitch, to.gazePitch, t);
  p.split = lerpf(from.split, to.split, t);
  p.eyeW = lerpf(from.eyeW, to.eyeW, t);
  p.eyeH = lerpf(from.eyeH, to.eyeH, t);
  p.eyeTilt = lerpf(from.eyeTilt, to.eyeTilt, t);
  p.eyeOpen = lerpf(from.eyeOpen, to.eyeOpen, t);
  p.r = lerpf(from.r, to.r, t);
  p.g = lerpf(from.g, to.g, t);
  p.b = lerpf(from.b, to.b, t);
  p.gr = lerpf(from.gr, to.gr, t);
  p.gg = lerpf(from.gg, to.gg, t);
  p.gb = lerpf(from.gb, to.gb, t);
  p.hasGlow = to.hasGlow;
  return p;
}

// toPoints + fill: sample the silhouette and fill as a triangle fan from the
// center (valid because all profiles are star-shaped).
void FinagotchiPet::drawSilhouette(const Pose& p, float cx, float cy,
                                   float sx, float sy, float scale,
                                   uint16_t color) {
  for (int i = 0; i < NRAD; i++) {
    float theta = (float)i * (TWO_PI / (float)NRAD);
    float r = p.radii[i] * scale;
    px[i] = cx + cosf(theta) * r * sx;
    py[i] = cy + sinf(theta) * r * sy;
  }
  int icx = (int)cx, icy = (int)cy;
  for (int i = 0; i < NRAD; i++) {
    int j = (i + 1) % NRAD;
    spr->fillTriangle(icx, icy, (int)px[i], (int)py[i],
                      (int)px[j], (int)py[j], color);
  }
}

// Capsule (stadium) — equivalent of capsulePath from shape.ts, filled.
// w/h are the full width/height; the capsule axis follows the longer side.
void FinagotchiPet::drawCapsule(float cx, float cy, float w, float h,
                                float tiltDeg, uint16_t color) {
  if (w < 0.5f) w = 0.5f;
  if (h < 0.5f) h = 0.5f;

  // Normalize so the capsule lies along local +x.
  float phi = tiltDeg * DEG_TO_RAD;
  if (h > w) {
    float tmp = w; w = h; h = tmp;
    phi += PI / 2.0f;
  }
  float r = h * 0.5f;
  float L = (w - h) * 0.5f;   // half straight length

  float cp = cosf(phi), sp = sinf(phi);

  const int N = 18;           // boundary samples
  float ex[N], ey[N];
  int n = 0;
  // right semicircle: -90 .. +90 deg
  for (int i = 0; i < N / 2; i++) {
    float a = (-90.0f + 180.0f * (float)i / (float)(N / 2)) * DEG_TO_RAD;
    float lx = L + r * cosf(a);
    float ly = r * sinf(a);
    ex[n] = cx + lx * cp - ly * sp;
    ey[n] = cy + lx * sp + ly * cp;
    n++;
  }
  // left semicircle: +90 .. +270 deg
  for (int i = 0; i < N / 2; i++) {
    float a = (90.0f + 180.0f * (float)i / (float)(N / 2)) * DEG_TO_RAD;
    float lx = -L + r * cosf(a);
    float ly = r * sinf(a);
    ex[n] = cx + lx * cp - ly * sp;
    ey[n] = cy + lx * sp + ly * cp;
    n++;
  }

  int icx = (int)cx, icy = (int)cy;
  for (int i = 0; i < n; i++) {
    int j = (i + 1) % n;
    spr->fillTriangle(icx, icy, (int)ex[i], (int)ey[i],
                      (int)ex[j], (int)ey[j], color);
  }
}

void FinagotchiPet::render(float nowSec) {
  if (!spr || !spr->created()) return;

  Pose pose = poseAt(nowSec);

  // --- Liveliness (pure functions of time, standing in for face.ts) ---
  // Breathing: subtle vertical squash (sil.sy *= breath).
  float breath = 1.0f + 0.018f * sinf(TWO_PI * nowSec / 3.2f);

  // Wander gaze: slow incommensurate sines.
  float wanderYaw   = 14.0f * sinf(TWO_PI * nowSec / 7.3f)
                    +  6.0f * sinf(TWO_PI * nowSec / 2.9f + 1.7f);
  float wanderPitch =  8.0f * sinf(TWO_PI * nowSec / 5.1f + 1.3f);

  // Blink: quick lid close every ~3.9 s.
  float blinkPhase = fmodf(nowSec, 3.9f);
  float lid = 1.0f;
  if (blinkPhase < 0.16f) {
    float bt = blinkPhase / 0.16f;              // 0 -> 1
    lid = 1.0f - 0.95f * (1.0f - fabsf(2.0f * bt - 1.0f));
  }

  float gazeYaw = pose.gazeYaw + wanderYaw;
  float gazePitch = pose.gazePitch + wanderPitch;

  uint16_t bodyColor = spr->color565((uint8_t)pose.r, (uint8_t)pose.g, (uint8_t)pose.b);
  uint16_t glowColor = spr->color565((uint8_t)pose.gr, (uint8_t)pose.gg, (uint8_t)pose.gb);

  float sw = (float)spr->width();
  float sh = (float)spr->height();
  float cx = sw * 0.5f + pose.offX * R;
  float cy = sh * 0.5f + pose.offY * R;

  spr->fillSprite(TFT_BLACK);

  // Glow halo: same silhouette, slightly larger, behind the body.
  if (pose.hasGlow) {
    drawSilhouette(pose, cx, cy, 1.0f, breath, R * 1.09f, glowColor);
  }
  drawSilhouette(pose, cx, cy, 1.0f, breath, R, bodyColor);

  // Eyes: placed relative to the local contour (like bodyRadius fitting in
  // engine.ts), so they track the body through morphs.
  float rSide = radiusAt(pose.radii, 0.0f);             // east
  float rTop  = radiusAt(pose.radii, 3.0f * PI / 2.0f); // north
  float eyeDX = 0.30f * rSide * R * (pose.split / 14.0f);
  float eyeY  = cy - 0.25f * rTop * R * breath + gazePitch * 0.006f * R;
  float shiftX = gazeYaw * 0.007f * R;

  // Fake head-turn depth: the far eye shrinks as |yaw| grows.
  float farShrink = 1.0f - clampf(fabsf(gazeYaw) / 25.0f, 0.0f, 1.0f) * 0.3f;

  float lidOpen = fminf(lid, pose.eyeOpen);
  float wNear = pose.eyeW * R;
  float hNear = pose.eyeH * R * lidOpen;
  float wFar = wNear * farShrink;
  float hFar = hNear * farShrink;

  bool gazeRight = gazeYaw >= 0.0f;
  drawCapsule(cx - eyeDX + shiftX, eyeY,
              gazeRight ? wFar : wNear, gazeRight ? hFar : hNear,
              pose.eyeTilt, TFT_BLACK);
  drawCapsule(cx + eyeDX + shiftX, eyeY,
              gazeRight ? wNear : wFar, gazeRight ? hNear : hFar,
              pose.eyeTilt, TFT_BLACK);

  // Center the sprite if it fell back to a smaller size.
  int sx = (tft->width() - spr->width()) / 2;
  int sy = (tft->height() - spr->height()) / 2;
  spr->pushSprite(sx, sy);
}
