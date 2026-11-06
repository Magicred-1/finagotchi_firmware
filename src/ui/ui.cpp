/*
  ui.cpp — bridge implementation (see ui.h): shared UI state, screen/timer
  bring-up, and the chrome setters main.cpp calls. Overlays (timed status
  messages + the BLE pairing passkey panel) live on lv_layer_top() so they
  ride over either screen.
*/

#include "ui_internal.h"

#ifndef AFK_TIMEOUT_MS
#define AFK_TIMEOUT_MS 60000   // idle time before the pet naps
#endif
#ifndef SCREEN_SLEEP_TIMEOUT_MS
#define SCREEN_SLEEP_TIMEOUT_MS 120000   // idle time before the display sleeps
#endif

UiState g_ui;

namespace {

// --- overlays (lv_layer_top) --------------------------------------------------

lv_obj_t* overlayBox;
lv_obj_t* overlayLabel;
lv_timer_t* overlayTimer;

lv_obj_t* passkeyBox;
lv_obj_t* passkeyNum;

lv_obj_t* wifiSetupBox;   // join progress: spinner + caption

void overlayHide(lv_timer_t*) {
  lv_obj_set_hidden(overlayBox, true);
}

void overlayInit() {
  overlayBox = lv_obj_create(lv_layer_top());
  lv_obj_set_size(overlayBox, 188, 64);
  lv_obj_align(overlayBox, LV_ALIGN_CENTER, 0, 0);
  uiThemeDialog(overlayBox);
  lv_obj_set_style_pad_all(overlayBox, 4, 0);
  overlayLabel = lv_label_create(overlayBox);
  uiThemeTextBounds(overlayLabel, 172, 52, true);
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
  uiThemeDialog(passkeyBox);

  lv_obj_t* cap = lv_label_create(passkeyBox);
  lv_obj_set_style_text_font(cap, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(cap, UI_COL_EYEBROW, 0);
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

  // Wi-Fi join progress: built hidden, shown around the blocking join in
  // main.cpp (processProvision). The join pumps lv_timer_handler so the
  // spinner actually spins.
  wifiSetupBox = lv_obj_create(lv_layer_top());
  lv_obj_set_size(wifiSetupBox, 150, 116);
  lv_obj_align(wifiSetupBox, LV_ALIGN_CENTER, 0, 0);
  uiThemeDialog(wifiSetupBox);

  lv_obj_t* spin = lv_spinner_create(wifiSetupBox);
  lv_obj_set_size(spin, 44, 44);
  lv_obj_align(spin, LV_ALIGN_TOP_MID, 0, 12);
  lv_obj_set_style_arc_color(spin, UI_COL_PRIMARY, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(spin, UI_COL_SURFACE, LV_PART_MAIN);
  lv_obj_set_style_arc_width(spin, 4, LV_PART_MAIN);
  lv_obj_set_style_arc_width(spin, 4, LV_PART_INDICATOR);

  lv_obj_t* setupCap = lv_label_create(wifiSetupBox);
  lv_obj_set_style_text_font(setupCap, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(setupCap, UI_COL_MUTED, 0);
  lv_label_set_text(setupCap, "joining Wi-Fi...");
  lv_obj_align(setupCap, LV_ALIGN_BOTTOM_MID, 0, -12);

  lv_obj_set_hidden(wifiSetupBox, true);
}

// 1 s tick: refresh the countdown labels on the visible screen(s).
void secTick(lv_timer_t*) {
  uiScreenPetTick();
  uiScreenDcaTick();
  uiScreenNearbyTick();
}

// --- AFK sleep ----------------------------------------------------------------
//
// No screen switch: the current screen stays loaded (lock pad / OTA confirm /
// menu all survive the nap) — the pet just goes sleepy behind floating "z"s on
// lv_layer_top, and main.cpp dims the backlight + slows the scene on
// ui::afkActive(). The waking input is consumed (see input.cpp) so a sleepy
// prod never triggers an action.

uint32_t   afkLastActivity = 0;
bool       afkOn = false;
bool       displayOff = false;
PetMoodId  afkStashMood = PetMoodId::MOOD_CALM;
lv_obj_t*  afkZzz = nullptr;
lv_timer_t* afkZzzTimer = nullptr;
uint8_t    afkZzzPhase = 0;

void afkZzzTick(lv_timer_t*) {
  static const char* phases[4] = { "", "z", "z z", "z z z" };
  afkZzzPhase = (afkZzzPhase + 1) % 4;
  lv_label_set_text(afkZzz, phases[afkZzzPhase]);
}

void afkEnter() {
  afkOn = true;
  afkStashMood = g_ui.pet->mood();
  g_ui.pet->setMood(PetMoodId::MOOD_SLEEPY, millis() / 1000.0f);

  afkZzz = lv_label_create(lv_layer_top());
  lv_obj_set_style_text_font(afkZzz, &lv_font_montserrat_16, 0);
  lv_obj_set_style_text_color(afkZzz, UI_COL_MUTED, 0);
  lv_obj_align(afkZzz, LV_ALIGN_CENTER, 24, -64);   // drifting above the pet
  lv_label_set_text(afkZzz, "");
  afkZzzPhase = 0;
  afkZzzTimer = lv_timer_create(afkZzzTick, 600, nullptr);
  Serial.println("AFK: pet napping");
}

void afkExit() {
  afkOn = false;
  g_ui.pet->setMood(afkStashMood, millis() / 1000.0f);
  if (afkZzzTimer) { lv_timer_delete(afkZzzTimer); afkZzzTimer = nullptr; }
  if (afkZzz) { lv_obj_delete(afkZzz); afkZzz = nullptr; }
  Serial.println("AFK: awake");
}

void displayWake() {
  if (!displayOff) return;
  displayOff = false;
  g_ui.tft->writecommand(0x11);   // GC9A01 SLPOUT
  delay(120);
  lv_obj_invalidate(lv_screen_active());
  lv_obj_invalidate(lv_layer_top());
  Serial.println("DISPLAY: awake");
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
  afkLastActivity = millis();
  g_ui.ready = true;
  Serial.println("UI: LVGL ready (pet canvas + chrome)");
}

void ui::update() {
  uiInputUpdate();
  if (displayOff) {
    delay(20);
    return;
  }
  // Idle -> nap. Never while the pairing passkey panel is up: it must stay
  // readable for the phone, and the backlight dimming would fight it.
  if (!afkOn && millis() - afkLastActivity >= AFK_TIMEOUT_MS &&
      lv_obj_is_hidden(passkeyBox)) {
    afkEnter();
  }
  if (millis() - afkLastActivity >= SCREEN_SLEEP_TIMEOUT_MS &&
      lv_obj_is_hidden(passkeyBox)) {
    screenSleep();
    return;
  }
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

void ui::setAppConnected(bool connected) {
  if (!g_ui.ready) return;
  uiScreenPetSetConnected(connected);
}

void ui::setBattery(uint8_t pct) {
  if (!g_ui.ready) return;
  uiScreenPetSetBattery(pct);
}

void ui::setWifiOnline(bool online) {
  if (!g_ui.ready) return;
  uiScreenPetSetWifiOnline(online);
  uiScreenDcaSetWifiOnline(online);
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
  if (afkOn) { afkLastActivity = millis(); afkExit(); }   // never hide a toast
  uiScreenPetToast(text, false);
}

void ui::enqueueRewardToast(const char* text) {
  if (!g_ui.ready) return;
  if (afkOn) { afkLastActivity = millis(); afkExit(); }
  uiScreenPetToast(text, true);   // dca:hit: app amber, the "magic moment"
}

void ui::setMenuAccessory(const char* name) {
  if (!g_ui.ready) return;
  uiScreenMenuSetAccessory(name);
}

void ui::setMenuMood(const char* name) {
  if (!g_ui.ready) return;
  uiScreenMenuSetMood(name);
}

void ui::setMenuBackdrop(const char* name) {
  if (!g_ui.ready) return;
  uiScreenMenuSetBackdrop(name);
}

void ui::setMenuLock(const char* name) {
  if (!g_ui.ready) return;
  uiScreenMenuSetLock(name);
}

void ui::setMenuWifi(const char* name) {
  if (!g_ui.ready) return;
  uiScreenMenuSetWifi(name);
}

void ui::setConnectStatus(const char* text) {
  if (!g_ui.ready) return;
  uiScreenConnectSetStatus(text);
}

void ui::showLock(int mode) {
  if (!g_ui.ready) return;
  uiScreenLockShow(mode);
}

bool ui::lockActive() {
  return g_ui.ready && uiScreenLockActive();
}

void ui::setLocked(bool on) {
  g_ui.locked = on;
}

void ui::setCreateTickers(const char* const* tickers, uint8_t n) {
  if (!g_ui.ready) return;
  uiScreenCreateSetTickers(tickers, n);
}

void ui::showOverlay(const char* msg, uint32_t ms) {
  if (!g_ui.ready) return;
  lv_label_set_text(overlayLabel, msg);
  lv_obj_set_hidden(overlayBox, false);
  uiThemePress(overlayBox);
  lv_timer_set_period(overlayTimer, ms);
  lv_timer_set_repeat_count(overlayTimer, 1);
  lv_timer_resume(overlayTimer);
  lv_timer_reset(overlayTimer);
}

void ui::hideOverlay() {
  if (!g_ui.ready) return;
  lv_timer_pause(overlayTimer);
  lv_obj_set_hidden(overlayBox, true);
}

void ui::showWifiSetup(bool on) {
  if (!g_ui.ready) return;
  if (on) {
    // Win the z-order on lv_layer_top() (toasts would cover it otherwise).
    lv_obj_move_to_index(wifiSetupBox, -1);
    lv_obj_set_hidden(wifiSetupBox, false);
  } else {
    lv_obj_set_hidden(wifiSetupBox, true);
  }
}

void ui::pump() {
  if (!g_ui.ready) return;
  lv_timer_handler();   // animations/timers only — no input poll
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
  uiThemeFocusPop(passkeyBox);
}

void ui::hidePasskey() {
  lv_obj_set_hidden(passkeyBox, true);
}

void ui::showOtaConfirm(const char* newVersion, uint32_t size) {
  if (!g_ui.ready) return;
  uiScreenOtaShow(newVersion, size);
}

bool ui::afkActive() {
  return afkOn;
}

void ui::screenSleep() {
  if (displayOff || !g_ui.ready) return;
  if (afkOn) afkExit();
  displayOff = true;
  g_ui.tft->writecommand(0x10);   // GC9A01 SLPIN; VCC backlight stays on
  delay(120);
  Serial.println("DISPLAY: sleeping (VCC backlight remains powered)");
}

bool ui::screenSleeping() {
  return displayOff;
}

bool ui::afkInput() {
  afkLastActivity = millis();
  bool woke = displayOff || afkOn;
  if (displayOff) displayWake();
  if (afkOn) afkExit();
  return woke;   // this input only woke the device — swallow the action
}
