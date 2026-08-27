/*
  FinagotchiPet — ESP32/TFT_eSPI port of the Finagotchi engine.

  Mirrors engine.ts: render(now) is a pure function of time. The only mutable
  state is the current lifecycle state and the timestamps of transitions, so
  morphs are deterministic.

  Silhouettes use the exact radial profiles from profiles.ts (64 samples,
  theta = 0 points right, growing clockwise in y-down screen coords).
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

class FinagotchiPet {
public:
  static const int NRAD = 64;       // PROFILE_SAMPLES

  // scale = base radius unit in pixels (matches engine.ts `scale`)
  void begin(TFT_eSPI* tft, float scale = 115.0f);
  void end();

  void setState(PetState id, float nowSec);
  bool evolve(float nowSec);        // egg -> coinling -> hodler -> whale
  PetState state() const { return cur; }

  void render(float nowSec);        // draw one frame

private:
  struct Pose {
    float radii[NRAD];
    float offX, offY;               // ball-radius units
    float gazeYaw, gazePitch;       // degrees
    float split;                    // eye separation units
    float eyeW, eyeH, eyeTilt, eyeOpen;
    float r, g, b;                  // body color 0-255
    float gr, gg, gb;               // glow color 0-255
    bool  hasGlow;
  };

  TFT_eSPI*   tft = nullptr;
  TFT_eSprite* spr = nullptr;
  float       R = 115.0f;

  PetState  cur = PET_EGG;
  PetState  prev = PET_EGG;
  float     tCur = 0.0f;
  float     tPrev = 0.0f;
  float     morphDur = 0.45f;

  float px[NRAD];
  float py[NRAD];

  Pose  poseFor(PetState id) const;
  Pose  poseAt(float nowSec) const;
  float radiusAt(const float* radii, float theta) const;

  void  drawSilhouette(const Pose& p, float cx, float cy,
                       float sx, float sy, float scale, uint16_t color);
  void  drawCapsule(float cx, float cy, float w, float h,
                    float tiltDeg, uint16_t color);
};
