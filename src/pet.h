/*
  FinagotchiPet — ESP32/TFT_eSPI port of the app's radial Finagotchi engine.

  Faithful ports:
  - profiles.ts radial bodies (64 samples) + morph = same-angle radii lerp
  - face.ts sphere-projected eyes (eyePoses tangent basis), seeded blink
    schedule (createRng 0x5eed), loopNoise liveliness
  - expressions.ts: 6 moods overriding gaze/split/eyes, blended over 0.45 s
  - engine.ts: dated setters, look target (mix/wander, 0.24 s morph),
    glow = body x1.15 @ 22% (PetBody), white eyes (PetEyes #f5f5f5)
  - PetAccessory.tsx collectibles: crown / glasses / bowtie / halo / diamond
  - PetCanvas reactions (jump/spin/glow/dance) + LevelUpAnimation sparkle
    burst on evolve

  render(now) is a pure function of time; setters are dated.
*/

#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>

enum PetState : uint8_t {
  PET_EGG = 0,
  PET_COINLING,
  PET_HODLER,
  PET_WHALE,
  PET_STATE_COUNT
};

// Emotions (BLE `mood:` id) — order matches expressions.ts EXPRESSIONS[].
enum PetMoodId : uint8_t {
  MOOD_CALM = 0,
  MOOD_HAPPY,
  MOOD_EXCITED,
  MOOD_WAITING,
  MOOD_SLEEPY,
  MOOD_SAD,
  MOOD_COUNT
};

// Collectibles (BLE `item:` id) — matches PetAccessory.tsx.
enum PetItem : uint8_t {
  ITEM_NONE = 0,
  ITEM_CROWN,
  ITEM_GLASSES,
  ITEM_BOWTIE,
  ITEM_HALO,
  ITEM_DIAMOND,
  ITEM_COUNT
};

// Reactions (BLE `react:`) — matches PetCanvas PetReaction.
enum PetReaction : uint8_t {
  REACT_NONE = 0,
  REACT_JUMP,
  REACT_SPIN,
  REACT_GLOW,
  REACT_DANCE
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
  void setMood(uint8_t mood, float nowSec);
  void setItem(uint8_t item);
  void react(uint8_t reaction, float nowSec);
  uint8_t mood() const { return curMood; }
  uint8_t item() const { return curItem; }

  // Bottom stats bar: streak days / points / happiness (0-100).
  void setStats(uint32_t streakDays, uint32_t points, uint8_t happiness);

  void render(float nowSec);        // draw one frame

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
  TFT_eSprite* spr = nullptr;
  float       R = 105.0f;

  PetState  cur = PET_EGG;
  PetState  prev = PET_EGG;
  float     tCur = 0.0f;

  // External gaze target with dated morph (0.24 s)
  float     lookYaw = 0.0f,    lookPitch = 0.0f;
  float     lookMix = 0.0f,    lookWander = 1.0f;
  float     lookPrevYaw = 0.0f, lookPrevPitch = 0.0f;
  float     lookPrevMix = 0.0f, lookPrevWander = 1.0f;
  float     lookAtT = -10.0f;

  // Expression (dated, blends over 0.45 s)
  uint8_t   curMood = MOOD_CALM;
  uint8_t   prevMood = MOOD_CALM;
  float     moodAtT = -10.0f;

  uint8_t   curItem = ITEM_NONE;

  // Reaction + level-up burst
  uint8_t   reaction = REACT_NONE;
  float     reactT = -10.0f;
  float     burstT = -10.0f;

  // Bottom stats bar
  uint32_t  statsStreak = 0;
  uint32_t  statsPoints = 0;
  uint8_t   statsHappy = 50;

  // screen-space draw buffers
  float dx[NRAD], dy[NRAD];

  Pose  poseFor(PetState id) const;
  Pose  poseAt(float nowSec) const;
  float radiusAt(const float* radii, float theta) const;

  void  fillPoly(uint16_t color);   // triangle fan over dx/dy
  void  mapPoint(float ux, float uy, float rotC, float rotS,
                 float scale, float cx, float cy, float& sx, float& sy) const;
  void  drawEye(const Pose& p, const EyePose& e, const EyeCfg& cfg,
                float lid, float rotC, float rotS, float scale,
                float cx, float cy, uint16_t bodyColor);
  void  drawItem(float rotC, float rotS, float scale, float cx, float cy);
  void  drawBurst(float nowSec, float cx, float cy);
  void  drawStatsBar();
};
