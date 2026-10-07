/*
  FinagotchiPet — ESP32/TFT_eSPI port of the app's radial Finagotchi engine.

  Faithful ports:
  - profiles.ts radial bodies (64 samples): procedural egg + GHOST_PROFILES
    hem/curl/arms ghost silhouettes on coinling/hodler/whale; morph =
    same-angle radii lerp from a departure-pose snapshot (engine.ts departFige)
  - face.ts sphere-projected eyes (eyePoses tangent basis), seeded blink
    schedule (createRng 0x5eed), loopNoise liveliness
  - expressions.ts: 6 moods overriding gaze/split/eyes, blended over 0.45 s
  - engine.ts: dated setters, look target (mix/wander, 0.24 s morph),
    glow = body x1.15 @ 22% over navy #07111F (PetBody), white eyes
    (PetEyes #f5f5f5)
  - PetAccessory.tsx collectibles: crown / glasses / bowtie / halo / diamond
  - PetCanvas reactions (jump/spin/glow/dance) + LevelUpAnimation sparkle
    burst on evolve

  render(now) is a pure function of time; setters are dated.

  Scene engine ONLY: all chrome (stats bar, battery, DCA views, toasts,
  sync beacon) lives in the LVGL UI layer (src/ui/). render() draws the
  scene into the TFT_eSprite and does NOT push it to the panel — the UI
  layer binds the framebuffer to an LVGL canvas and its flush owns the
  display.
*/

#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>
#include <memory>

enum class PetState : uint8_t {
  PET_EGG = 0,
  PET_COINLING,
  PET_HODLER,
  PET_WHALE,
  PET_STATE_COUNT
};

// Emotions (BLE `mood:` id) — order matches expressions.ts EXPRESSIONS[].
enum class PetMoodId : uint8_t {
  MOOD_CALM = 0,
  MOOD_HAPPY,
  MOOD_EXCITED,
  MOOD_WAITING,
  MOOD_SLEEPY,
  MOOD_SAD,
  MOOD_COUNT
};

// Collectibles (BLE `item:` id) — matches PetAccessory.tsx.
enum class PetItem : uint8_t {
  ITEM_NONE = 0,
  ITEM_CROWN,
  ITEM_GLASSES,
  ITEM_BOWTIE,
  ITEM_HALO,
  ITEM_DIAMOND,
  ITEM_TSHIRT,
  ITEM_COUNT
};

// Reactions (BLE `react:`) — matches PetCanvas PetReaction.
enum class PetReaction : uint8_t {
  REACT_NONE = 0,
  REACT_JUMP,
  REACT_SPIN,
  REACT_GLOW,
  REACT_DANCE
};

// Integer views of the *_COUNT sentinels: enum-class values don't convert
// implicitly, but array sizes and bounds checks need plain numbers.
constexpr size_t kPetStateCount = static_cast<size_t>(PetState::PET_STATE_COUNT);
constexpr size_t kMoodCount     = static_cast<size_t>(PetMoodId::MOOD_COUNT);
constexpr size_t kItemCount     = static_cast<size_t>(PetItem::ITEM_COUNT);

// Max mirrored DCA plans (BLE dca:plan slots, NVS blobs, portfolio cards).
constexpr size_t kDcaMaxPlans = 8;

// One DCA plan, mirrored read-only from the app (BLE dca: commands) or from
// the relay poll while offline. ticker is clamped to 6 chars + NUL.
// priceUsd = last known token unit price (app/relay-fed; 0 = unknown).
// holdingsHeld is a FLOAT (fractional tokens, e.g. 1.5) — same 4 bytes as
// the old uint32_t, so the NVS blob layout is unchanged in size.
struct DcaPlan {
  bool     enabled;
  uint32_t nextBuyEpoch;
  float    amountSol;
  char     ticker[7];
  uint32_t buys;
  float    holdingsHeld;
  float    priceUsd;
};

class FinagotchiPet {
public:
  static const int NRAD = 64;   // PROFILE_SAMPLES

  // scale = ball radius in px (app uses size/2; 105 fills a 240px screen)
  void begin(TFT_eSPI* tft, float scale = 105.0f);
  void end();

  void setState(PetState id, float nowSec);
  bool evolve(float nowSec);        // egg -> coinling -> hodler -> whale
  PetState state() const { return cur; }

  // App-driven gaze target (engine.ts setLook). clearLook() resumes wander.
  void setLook(float yawDeg, float pitchDeg, float nowSec);
  void clearLook(float nowSec);

  // Emotion + collectible + reaction (app-driven via BLE).
  void setMood(PetMoodId mood, float nowSec);
  void setItem(PetItem item);
  void react(PetReaction reaction, float nowSec);
  PetMoodId mood() const { return curMood; }
  PetItem item() const { return curItem; }

  // Waiting-for-sync mood side-effect (waiting mood while the device
  // advertises, previous mood restored after). The beacon visual is an
  // lv_spinner in the UI layer (ui::setSyncWait wraps both).
  void setSyncWait(bool on, float nowSec);
  bool syncWaiting() const { return syncWait; }

  // PetCanvas scene: two-zone sky/ground backdrop tinted per the app's
  // BACKGROUND_COLORS themes (rgba composites over #07111F), plus the
  // animated ground shadow that mirrors the idle float / reactions.
  // 6 themes, default = index 0 (the app default).
  void setSceneTheme(uint8_t id);
  uint8_t sceneTheme() const { return curTheme; }
  static const char* sceneThemeName(uint8_t id);
  static int sceneThemeForName(const char* name);   // -1 when unknown
  static constexpr uint8_t kSceneThemeCount = 6;

  // dca:hit celebration: LevelUpAnimation sparkle burst.
  void sparkleBurst(float nowSec) { burstT = nowSec; }

  void render(float nowSec);        // draw one frame into the sprite (no push)

  // LVGL canvas bridge: the sprite framebuffer the scene renders into.
  uint8_t* frameBuffer();
  int16_t  frameWidth() const;
  int16_t  frameHeight() const;

  // face.ts eyePoses output: position + tangent basis + depth
  struct EyePose { float x, y, a, b, c, d, depth; };

private:
  struct EyeCfg { float w, h, tilt, open; };

  struct Pose {
    float radii[NRAD];
    float offX, offY;               // ball-radius units
    float gazeYaw, gazePitch, gazeRoll;
    float split;
    EyeCfg eyes[2];
    float eyeAlpha, bodyAlpha;
    float fr, fg, fb;               // body fill
    float gr, gg, gb;               // glow
    bool  hasGlow;
  };

  TFT_eSPI*   tft = nullptr;
  std::unique_ptr<TFT_eSprite> spr;
  float       R = 105.0f;

  PetState  cur = PetState::PET_EGG;
  float     tCur = 0.0f;

  // Departure pose snapshot (engine.ts departFige): the pose visible at the
  // moment of setState, so morphs interrupted mid-transition never pop.
  Pose      fromPose;
  bool      hasFrom = false;

  // External gaze target with dated morph (0.24 s)
  float     lookYaw = 0.0f,    lookPitch = 0.0f;
  float     lookMix = 0.0f,    lookWander = 1.0f;
  float     lookPrevYaw = 0.0f, lookPrevPitch = 0.0f;
  float     lookPrevMix = 0.0f, lookPrevWander = 1.0f;
  float     lookAtT = -10.0f;

  // Expression (dated, blends over 0.45 s)
  PetMoodId curMood = PetMoodId::MOOD_CALM;
  PetMoodId prevMood = PetMoodId::MOOD_CALM;
  float     moodAtT = -10.0f;

  PetItem   curItem = PetItem::ITEM_NONE;

  // Reaction + level-up burst
  PetReaction reaction = PetReaction::REACT_NONE;
  float     reactT = -10.0f;
  float     burstT = -10.0f;

  // Waiting-for-sync mood side-effect
  bool      syncWait = false;
  PetMoodId preSyncMood = PetMoodId::MOOD_CALM;

  // Scene backdrop theme (PetCanvas BACKGROUND_COLORS composites, cached)
  uint8_t   curTheme = 0;
  uint16_t  skyColor = 0, groundColor = 0;
  uint8_t   groundR = 0, groundG = 0, groundB = 0;   // for the shadow blend

  // screen-space draw buffers
  float dx[NRAD], dy[NRAD];

  Pose  poseFor(PetState id) const;
  Pose  poseAt(float nowSec) const;
  float radiusAt(const float* radii, float theta) const;

  void  fillPoly(uint16_t color);   // triangle fan over dx/dy
  void  fillPolyN(const float* xs, const float* ys, int n, uint16_t color);
  void  mapPoint(float ux, float uy, float rotC, float rotS,
                 float scale, float cx, float cy, float& sx, float& sy) const;
  void  drawEye(const Pose& p, const EyePose& e, const EyeCfg& cfg,
                float lid, float rotC, float rotS, float scale,
                float cx, float cy, uint16_t bodyColor);

  // 2D affine anchor matrix [a,b,c,d,e,f] (engine.ts composeAnchors).
  // Artwork is drawn around the origin in units of R and mapped through it.
  struct Anchor { float a, b, c, d, e, f; };

  void  anchorPoint(const Anchor& an, float ux, float uy,
                    float& sx, float& sy) const;
  bool  anchorFor(const Pose& p, const float eyeX[2], const float eyeY[2],
                  bool eyesOk, float gazeRoll, float breath,
                  float rotC, float rotS, float rRotDeg, float rScale,
                  float cx, float cy, Anchor& out) const;
  void  drawItem(const Anchor& an);   // anchored accessory artwork
  void  drawShirt(float cx, float cy);  // fitted tee from the body contour
  void  drawBurst(float nowSec, float cx, float cy);
  void  drawBackdrop();               // sky/ground zones (scene theme)
  void  drawShadow(float nowSec, float cx, float rScale);  // PetCanvas groundShadow
};
