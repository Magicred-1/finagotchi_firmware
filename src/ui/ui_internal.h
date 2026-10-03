/*
  ui_internal.h — shared state + cross-module hooks for the src/ui/ layer.
  Not part of the public bridge (that is ui.h); only the ui/*.cpp modules
  include this. Colors/radii/spacing come from theme.h (app design tokens).
*/

#pragma once

#include <lvgl.h>
#include "ui.h"
#include "theme.h"

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
  bool     amountInSol = false; // BTN1 double on the portfolio screen toggles
};
extern UiState g_ui;

// ui_util.cpp — formatting helpers ported from the old pet.cpp chrome.
void uiFmtVal(uint32_t v, char* buf, size_t n);
void uiFmtCountdown(uint32_t nextBuyEpoch, uint32_t nowEpoch, char* buf, size_t n);
void uiFmtAmount(const DcaPlan& p, char* buf, size_t n);   // honors amountInSol
void uiFmtUsd(double usd, char* buf, size_t n);            // "$1,234.56" / "$12.3k"
const char* uiStageName(uint8_t subStage);
bool uiPlanOverdue(uint8_t slot);   // flagged, or epoch simply passed
double uiPlanValueUsd(const DcaPlan& p);   // holdings x price; <=0 = unknown

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
void uiScreenPetToast(const char* text, bool reward); // toasts ride lv_layer_top()
void uiScreenPetPlansChanged();          // next-buy chip refresh
void uiScreenPetChipAdvance();           // BTN2 double: manual rotate
void uiScreenPetTick();                  // 1 s countdown refresh

// screen_dca.cpp (portfolio / positions)
lv_obj_t* uiScreenDcaCreate();
void uiScreenDcaShow();                  // BTN2 short on the pet screen
void uiScreenPetShow();                  // BTN2 long: back to the pet
void uiScreenDcaPlansChanged();
void uiScreenDcaHeaderChanged();         // totals: prices / SOL rate moved
void uiScreenDcaTick();                  // 1 s countdown refresh
bool uiScreenDcaDetailOpen();
void uiScreenDcaCloseDetail();
void uiScreenDcaToggleAmountUnit();
