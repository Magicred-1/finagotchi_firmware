/*
  ui_internal.h — shared state + cross-module hooks for the src/ui/ layer.
  Not part of the public bridge (that is ui.h); only the ui/*.cpp modules
  include this.
*/

#pragma once

#include <lvgl.h>
#include "ui.h"

// Palette (matches the app's #07111F navy scene).
#define UI_NAVY  lv_color_hex(0x07111F)
#define UI_TEXT  lv_color_hex(0xE6E6F0)
#define UI_DIM   lv_color_hex(0x50647A)
#define UI_LINE  lv_color_hex(0x2D2D3C)
#define UI_MINT  lv_color_hex(0x78FFD6)
#define UI_CYAN  lv_color_hex(0x22D3EE)
#define UI_AMBER lv_color_hex(0xFBBF24)
#define UI_PINK  lv_color_hex(0xF43F5E)
#define UI_BADGE lv_color_hex(0x1E2D46)

// Shared UI state, owned by ui.cpp.
struct UiState {
  TFT_eSPI*      tft = nullptr;
  FinagotchiPet* pet = nullptr;
  ui::Actions    actions = {};

  lv_obj_t*   petScreen = nullptr;
  lv_obj_t*   dcaScreen = nullptr;
  lv_group_t* group = nullptr;

  // DCA plan mirror (ui::setDcaPlan/clearDcaPlans)
  DcaPlan  plans[kDcaMaxPlans] = {};
  bool     overdue[kDcaMaxPlans] = {};
  uint8_t  planCount = 0;

  uint32_t epoch = 0;          // wall clock; 0 = never synced
  float    solUsd = 0.0f;      // SOL/USD; <= 0 = unknown
  bool     amountInSol = false; // BTN1 double on the DCA screen toggles
};
extern UiState g_ui;

// ui_util.cpp — formatting helpers ported from the old pet.cpp chrome.
void uiFmtVal(uint32_t v, char* buf, size_t n);
void uiFmtCountdown(uint32_t nextBuyEpoch, uint32_t nowEpoch, char* buf, size_t n);
void uiFmtAmount(const DcaPlan& p, char* buf, size_t n);   // honors amountInSol
const char* uiStageName(uint8_t subStage);
bool uiPlanOverdue(uint8_t slot);   // flagged, or epoch simply passed

// display.cpp
void uiDisplayInit(TFT_eSPI* tft);

// input.cpp
void uiInputInit();
void uiInputUpdate();   // poll buttons; called from ui::update()

// screen_pet.cpp
lv_obj_t* uiScreenPetCreate();
void uiScreenPetFrame(float nowSec);     // pet.render + canvas invalidate
void uiScreenPetSyncWait(bool on);
void uiScreenPetSetStats(uint32_t streakDays, uint32_t points, uint8_t happiness);
void uiScreenPetSetSubStage(uint8_t subStage);
void uiScreenPetSetBattery(int pct);     // <0 hides (USB power)
void uiScreenPetToast(const char* text); // toasts ride lv_layer_top()
void uiScreenPetPlansChanged();          // next-buy chip refresh
void uiScreenPetChipAdvance();           // BTN2 double: manual rotate
void uiScreenPetTick();                  // 1 s countdown refresh

// screen_dca.cpp
lv_obj_t* uiScreenDcaCreate();
void uiScreenDcaShow();                  // BTN2 short on the pet screen
void uiScreenPetShow();                  // BTN2 long: back to the pet
void uiScreenDcaPlansChanged();
void uiScreenDcaTick();                  // 1 s countdown refresh
bool uiScreenDcaDetailOpen();
void uiScreenDcaCloseDetail();
void uiScreenDcaToggleAmountUnit();
