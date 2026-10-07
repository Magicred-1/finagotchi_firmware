/*
  ui.cpp — bridge implementation (see ui.h): shared UI state, screen/timer
  bring-up, and the chrome setters main.cpp calls. Overlays (timed status
  messages + the BLE pairing passkey panel) live on lv_layer_top() so they
  ride over either screen.
*/

#include "ui_internal.h"

UiState g_ui;

namespace {

// --- overlays (lv_layer_top) --------------------------------------------------

lv_obj_t* overlayBox;
lv_obj_t* overlayLabel;
lv_timer_t* overlayTimer;

lv_obj_t* passkeyBox;
lv_obj_t* passkeyNum;

void overlayHide(lv_timer_t*) {
  lv_obj_set_hidden(overlayBox, true);
}

void overlayInit() {
  overlayBox = lv_obj_create(lv_layer_top());
  lv_obj_set_size(overlayBox, 180, 48);
  lv_obj_align(overlayBox, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_bg_color(overlayBox, UI_COL_BG, 0);
  lv_obj_set_style_bg_opa(overlayBox, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(overlayBox, 8, 0);
  lv_obj_set_style_border_width(overlayBox, 1, 0);
  lv_obj_set_style_border_color(overlayBox, UI_COL_PRIMARY, 0);
  lv_obj_set_style_pad_all(overlayBox, 4, 0);
  lv_obj_set_scrollable(overlayBox, false);
  overlayLabel = lv_label_create(overlayBox);
  lv_obj_set_style_text_color(overlayLabel, UI_COL_TEXT, 0);
  lv_obj_set_style_text_align(overlayLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_center(overlayLabel);
  lv_obj_set_hidden(overlayBox, true);
  overlayTimer = lv_timer_create(overlayHide, 2500, nullptr);
  lv_timer_set_auto_delete(overlayTimer, false);   // pause when done, don't delete
  lv_timer_set_repeat_count(overlayTimer, 1);
  lv_timer_pause(overlayTimer);

  passkeyBox = lv_obj_create(lv_layer_top());
  lv_obj_set_size(passkeyBox, 160, 100);
  lv_obj_align(passkeyBox, LV_ALIGN_CENTER, 0, -20);
  lv_obj_set_style_bg_color(passkeyBox, UI_COL_BG, 0);
  lv_obj_set_style_bg_opa(passkeyBox, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(passkeyBox, 8, 0);
  lv_obj_set_style_border_width(passkeyBox, 1, 0);
  lv_obj_set_style_border_color(passkeyBox, UI_COL_PRIMARY, 0);
  lv_obj_set_scrollable(passkeyBox, false);

  lv_obj_t* cap = lv_label_create(passkeyBox);
  lv_obj_set_style_text_font(cap, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(cap, UI_COL_PRIMARY, 0);
  lv_label_set_text(cap, "pairing code");
  lv_obj_align(cap, LV_ALIGN_TOP_MID, 0, 6);

  passkeyNum = lv_label_create(passkeyBox);
  lv_obj_set_style_text_font(passkeyNum, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(passkeyNum, UI_COL_TEXT, 0);
  lv_obj_align(passkeyNum, LV_ALIGN_CENTER, 0, 0);

  lv_obj_t* sub = lv_label_create(passkeyBox);
  lv_obj_set_style_text_font(sub, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(sub, UI_COL_MUTED, 0);
  lv_label_set_text(sub, "enter it in the app");
  lv_obj_align(sub, LV_ALIGN_BOTTOM_MID, 0, -6);

  lv_obj_set_hidden(passkeyBox, true);
}

// 1 s tick: refresh the countdown labels on the visible screen(s).
void secTick(lv_timer_t*) {
  uiScreenPetTick();
  uiScreenDcaTick();
}

} // namespace

void uiGroupSet(lv_obj_t* const* objs, uint8_t n, bool keepFocus) {
  lv_obj_t* prev = keepFocus ? lv_group_get_focused(g_ui.group) : nullptr;
  lv_group_remove_all_objs(g_ui.group);
  bool refocused = false;
  for (uint8_t i = 0; i < n; i++) {
    lv_group_add_obj(g_ui.group, objs[i]);
    if (objs[i] == prev) {
      lv_group_focus_obj(objs[i]);
      refocused = true;
    }
  }
  if (!refocused && n > 0) lv_group_focus_obj(objs[0]);
}

void ui::begin(TFT_eSPI* tft, FinagotchiPet* pet, const Actions& actions) {
  g_ui.pet = pet;
  g_ui.actions = actions;

  lv_init();
  uiDisplayInit(tft);
  uiThemeInit();
  g_ui.group = lv_group_create();
  g_ui.petScreen = uiScreenPetCreate();
  g_ui.dcaScreen = uiScreenDcaCreate();
  // menuScreen / createScreen stay null: their widgets are built lazily on
  // first show (and freed again on exit) — eager creation here exhausted
  // the heap at boot (BLE2902 + sprite + LVGL) and looped the device.
  uiInputInit();
  overlayInit();

  lv_screen_load(g_ui.petScreen);
  lv_timer_create(secTick, 1000, nullptr);
  g_ui.ready = true;
  Serial.println("UI: LVGL ready (pet canvas + chrome)");
}

void ui::update() {
  uiInputUpdate();
  lv_timer_handler();
}

void ui::renderPetFrame(float nowSec) {
  uiScreenPetFrame(nowSec);
}

void ui::setStats(uint32_t streakDays, uint32_t points, uint8_t happiness) {
  if (!g_ui.ready) return;
  uiScreenPetSetStats(streakDays, points, happiness);
}

void ui::setSubStage(uint8_t subStage) {
  if (!g_ui.ready) return;
  uiScreenPetSetSubStage(subStage);
}

void ui::setBattery(uint8_t pct) {
  if (!g_ui.ready) return;
  uiScreenPetSetBattery(pct);
}

void ui::clearBattery() {
  if (!g_ui.ready) return;
  uiScreenPetSetBattery(-1);
}

void ui::setSyncWait(bool on) {
  if (!g_ui.ready) return;
  // The pet keeps the mood side-effect; the beacon visual is the spinner.
  g_ui.pet->setSyncWait(on, millis() / 1000.0f);
  uiScreenPetSyncWait(on);
}

void ui::setDcaPlan(uint8_t idx, const DcaPlan& p, bool overdue) {
  if (idx >= kDcaMaxPlans) return;
  g_ui.plans[idx] = p;
  g_ui.plans[idx].ticker[sizeof(g_ui.plans[idx].ticker) - 1] = 0;
  g_ui.overdue[idx] = overdue;
  if (idx >= g_ui.planCount) g_ui.planCount = idx + 1;
  if (g_ui.ready) uiScreenDcaPlansChanged();
}

void ui::clearDcaPlans() {
  memset(g_ui.plans, 0, sizeof(g_ui.plans));
  memset(g_ui.overdue, 0, sizeof(g_ui.overdue));
  g_ui.planCount = 0;
  if (g_ui.ready) uiScreenDcaPlansChanged();
}

void ui::setEpoch(uint32_t epoch) {
  if (epoch == g_ui.epoch) return;
  g_ui.epoch = epoch;
  if (g_ui.ready) secTick(nullptr);   // countdowns track the new clock now
}

void ui::setSolUsd(float rate) {
  g_ui.solUsd = rate;
  if (rate <= 0.0f && g_ui.amountInSol) g_ui.amountInSol = false;
  if (g_ui.ready) uiScreenDcaHeaderChanged();   // SOL equivalent + labels
}

void ui::enqueueToast(const char* text) {
  if (!g_ui.ready) return;
  uiScreenPetToast(text, false);
}

void ui::enqueueRewardToast(const char* text) {
  if (!g_ui.ready) return;
  uiScreenPetToast(text, true);   // dca:hit: app purple, the "magic moment"
}

void ui::setMenuAccessory(const char* name) {
  if (!g_ui.ready) return;
  uiScreenMenuSetAccessory(name);
}

void ui::setMenuMood(const char* name) {
  if (!g_ui.ready) return;
  uiScreenMenuSetMood(name);
}

void ui::setCreateTickers(const char* const* tickers, uint8_t n) {
  if (!g_ui.ready) return;
  uiScreenCreateSetTickers(tickers, n);
}

void ui::showOverlay(const char* msg, uint32_t ms) {
  if (!g_ui.ready) return;
  lv_label_set_text(overlayLabel, msg);
  lv_obj_set_hidden(overlayBox, false);
  lv_timer_set_period(overlayTimer, ms);
  lv_timer_set_repeat_count(overlayTimer, 1);
  lv_timer_resume(overlayTimer);
  lv_timer_reset(overlayTimer);
}

void ui::showPasskey(uint32_t passkey) {
  if (!g_ui.ready) return;
  char num[8];
  snprintf(num, sizeof(num), "%06lu", static_cast<unsigned long>(passkey));
  lv_label_set_text(passkeyNum, num);
  // Win the z-order on lv_layer_top(): toasts/overlays created later would
  // otherwise render on top of the pairing panel.
  lv_obj_move_to_index(passkeyBox, -1);
  lv_obj_set_hidden(passkeyBox, false);
}

void ui::hidePasskey() {
  lv_obj_set_hidden(passkeyBox, true);
}
