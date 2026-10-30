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
  void (*syncNow)(float nowSec);        // confirm on the pet screen
  void (*cycleReaction)(float nowSec);  // menu "Play" row
  void (*feedPet)(float nowSec);        // menu "Feed pet" (BLE feed:req)
  void (*cycleItem)();                  // menu accessory row (item: push)
  void (*cycleMood)(float nowSec);      // menu mood row (mood: push)
  void (*cycleBackdrop)();              // menu backdrop row (scene theme cycle)
  void (*togglePause)(uint8_t slot);    // detail confirm (BLE dca:pause:)
  // Create-screen confirm; returns true when the request went out (app
  // connected) so the UI can pop back to the portfolio.
  bool (*createPlan)(const char* ticker, float amountSol, uint32_t freqSec);
  void (*parentalLock)();               // menu "Code lock" row (set/remove code)
  bool (*pinCheck)(uint16_t pin);       // lock screen: validate entered code
  void (*pinStore)(uint16_t pin);       // lock screen: save new code
  void (*pinClear)();                   // lock screen: remove code
  void (*checkUpdate)();                // menu "Update" row + BLE ota:start
  void (*otaConfirm)();                 // OTA confirm screen: push = schedule + reboot
  void (*otaCancel)();                  // OTA confirm screen: cancel (BLE status)
  uint32_t (*pinLockoutMs)();           // lock screen: ms left in a wrong-code lockout
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
// Reward variant is app amber — the dca:hit "magic moment".
void enqueueToast(const char* text);
void enqueueRewardToast(const char* text);

// Menu screen row labels (main.cpp keeps them in sync from blePushState).
void setMenuAccessory(const char* name);
void setMenuMood(const char* name);
void setMenuBackdrop(const char* name);

// Create-plan screen: the tickers the user can pick (main's xStocks table).
void setCreateTickers(const char* const* tickers, uint8_t n);

// Timed status overlay (center of screen, auto-hides after ms).
void showOverlay(const char* msg, uint32_t ms);
void hideOverlay();

// Wi-Fi join progress: spinning arc + caption on lv_layer_top while a
// provisioning join blocks the loop (processProvision). ui::pump() keeps the
// spinner animating through the blocking wait loops in main.cpp.
void showWifiSetup(bool on);
void pump();   // lv_timer_handler only — safe inside blocking join waits

// Parental lock (optional 4-digit code — main.cpp owns the NVS state).
// When a code is set the device boots locked: the pet animates but leaving
// the pet screen asks for the code. Modes: 0 unlock (validate), 1 set new
// (enter twice), 3 remove (validate then clear).
void showLock(int mode);
bool lockActive();               // lock screen is up (input routes to it)
void setLocked(bool on);         // boot state, from main's NVS load
void setMenuLock(const char* name);   // "on"/"off" info word cache
void setMenuWifi(const char* name);   // bounded Wi-Fi state for the menu row
void setConnectStatus(const char* text);  // Wi-Fi screen provisioning state

// BLE pairing passkey panel; hidden with hidePasskey() once pairing ends.
void showPasskey(uint32_t passkey);
void hidePasskey();

// OTA confirm screen (lazy build + free, like the menu/lock screens): the
// manifest said a newer firmware exists — show version + size, push =
// schedule the update and reboot into OTA mode, cancel = back to the menu.
void showOtaConfirm(const char* newVersion, uint32_t size);

// Top-right Wi-Fi status glyph (light blue online / muted offline). Driven by
// main.cpp's link tracking (noteWifiLink edge detector + WiFi event hook).
void setWifiOnline(bool online);

// AFK sleep: after AFK_TIMEOUT_MS (build flag, default 60 s) without local
// input the pet naps — MOOD_SLEEPY with floating "z z z" over the current
// screen, backlight dimmed + pet scene slowed by main.cpp. The first input
// after sleep only wakes (afkInput returns true, the action is consumed);
// toasts also wake so a dca:hit is never missed.
bool afkActive();
bool afkInput();   // input.cpp: note activity; true = it consumed a wake

// Boot recovery: both breadboard buttons, or a 4 s crown hold on PCB v1.
// Used to erase stale BLE bonds and the parental code — see WIRING.md.
bool recoveryRequested();

} // namespace ui
