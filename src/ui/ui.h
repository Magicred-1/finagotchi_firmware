/*
  ui.h — bridge between main.cpp and the LVGL UI layer (src/ui/).

  Every chrome setter main.cpp used to call on the pet object lives here now;
  the pet keeps only the scene engine (pet.h). Typical loop wiring:

    if (nowMs - lastFrame >= FRAME_MS) { lastFrame = nowMs; ui::renderPetFrame(nowSec); }
    ui::update();                    // buttons + lv_timer_handler()
*/

#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>
#include "../pet.h"

namespace ui {

// Semantic button actions owned by main.cpp (BLE sync / pet state pushes).
// Any may be nullptr.
struct Actions {
  void (*syncNow)(float nowSec);        // BTN1 short on the pet screen
  void (*cycleReaction)(float nowSec);  // BTN1 double on the pet screen
  void (*cycleMood)(float nowSec);      // BTN1 long (either screen)
};

void begin(TFT_eSPI* tft, FinagotchiPet* pet, const Actions& actions);
void update();                     // button poll + lv_timer_handler(); every loop pass
void renderPetFrame(float nowSec); // pet scene -> LVGL canvas; FRAME_MS cadence

// Bottom stats bar: streak days / points / happiness (0-100).
void setStats(uint32_t streakDays, uint32_t points, uint8_t happiness);

// 12-stage badge (1-12), shown top-left on the pet screen.
void setSubStage(uint8_t subStage);

// Top-right battery indicator (0-100). clearBattery() hides it (USB power).
void setBattery(uint8_t pct);
void clearBattery();

// Advertising scene: spinner on screen + waiting mood on the pet (the pet
// keeps the mood side-effect, the beacon visual is an lv_spinner).
void setSyncWait(bool on);

// DCA plan mirror: one slot per plan (0..kDcaMaxPlans-1), overdue marks the
// card/chip amber. Drives the next-buy chip and the DCA positions screen.
void setDcaPlan(uint8_t idx, const DcaPlan& p, bool overdue);
void clearDcaPlans();

// Wall clock for the countdown texts; 0 = never synced, shows "--".
void setEpoch(uint32_t epoch);

// SOL/USD rate for the amount unit toggle; <= 0 = unknown (USD only).
void setSolUsd(float rate);

// Gain/status toast ("+n TICKER", "synced", ...), floats up and fades.
void enqueueToast(const char* text);

// Timed status overlay (center of screen, auto-hides after ms).
void showOverlay(const char* msg, uint32_t ms);

// BLE pairing passkey panel; hidden with hidePasskey() once pairing ends.
void showPasskey(uint32_t passkey);
void hidePasskey();

} // namespace ui
