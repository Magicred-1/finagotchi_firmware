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
  bool           ready = false;   // ui::begin finished; setters are safe

  lv_obj_t*   petScreen = nullptr;
  lv_obj_t*   dcaScreen = nullptr;
  lv_obj_t*   menuScreen = nullptr;
  lv_obj_t*   createScreen = nullptr;
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
bool uiPlanPaused(const DcaPlan& p);  // disabled or epoch 0 -> PAUSED state
double uiPlanValueUsd(const DcaPlan& p);   // holdings x price; <=0 = unknown

// ui.cpp
// Rebuild the focus group to hold exactly the given objects (hidden cards
// must NEVER stay in the group — LV_KEY_NEXT would focus invisible cards).
// keepFocus preserves the focused object when it is still a member.
void uiGroupSet(lv_obj_t* const* objs, uint8_t n, bool keepFocus);

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
void uiScreenPetShow();                  // escape: back to the pet screen
void uiScreenDcaPlansChanged();
void uiScreenDcaHeaderChanged();         // totals: prices / SOL rate moved
void uiScreenDcaTick();                  // 1 s countdown refresh
bool uiScreenDcaDetailOpen();
void uiScreenDcaCloseDetail();
void uiScreenDcaToggleAmountUnit();
bool uiScreenDcaFocusAdvance();          // false: no more cards -> next screen
void uiScreenDcaTogglePause();           // detail double-press: pause/resume

// screen_create.cpp ("+ New plan" wheel: token -> amount -> frequency drums)
// Widgets are built lazily on first show and freed on exit (boot-heap
// discipline; see ui.cpp).
void uiScreenCreateShow();               // from the "+ New plan" card
void uiScreenCreateFreed();              // screen object deleted (auto_del)
void uiScreenCreateFocusAdvance();       // short: next option (wraps) / edit
void uiScreenCreateActivate(float nowSec); // long: confirm drum / CREATE
void uiScreenCreateSetTickers(const char* const* tickers, uint8_t n);

// screen_dca.cpp shared helper: token logo descriptor (nullptr = unknown).
const lv_image_dsc_t* uiTokenLogoDsc(const char* ticker);

// screen_menu.cpp (launcher app list) — lazy build + free, same pattern
void uiScreenMenuShow();                 // after the last portfolio card
void uiScreenMenuFreed();                // screen object deleted (auto_del)
void uiScreenMenuActivate(float nowSec); // long press: run the focused row
bool uiScreenMenuFocusAdvance();         // false: no more rows -> pet screen
void uiScreenMenuSetAccessory(const char* name);
void uiScreenMenuSetMood(const char* name);
