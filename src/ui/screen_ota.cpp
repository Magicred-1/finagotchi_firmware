/*
  screen_ota.cpp — firmware update confirm screen.

  Shown when the manifest says a newer firmware exists (menu "Update" row or
  the BLE ota:start command; the check itself lives in main.cpp). Static
  content only — there is nothing to navigate, so both input schemes drive
  it through the existing screen plumbing with no new input code:

    crown short / breadboard long = confirm (schedule + reboot into OTA mode)
    crown long / BTN2 short       = abort (actions.otaCancel, back to menu)

  Lazy build + free on exit (auto_del), same heap discipline as the menu and
  lock screens. Entering from the menu frees the menu; cancelling rebuilds
  it (uiScreenMenuShow frees this screen on the way out).
*/

#include "ui_internal.h"
#include "../version.h"

namespace {

lv_obj_t* otaScr;
lv_obj_t* versionLabel;
lv_obj_t* sizeLabel;
lv_obj_t* updateBtn;

// Layout (240 px round panel, every line kept inside the circle's chord):
//   y  40  download glyph in a primary chip
//   y  84  "Update available"            (16 pt)
//   y 108  "v1.0.0  ->  v1.1.0"            (20 pt, ellipsized to 180 px)
//   y 134  "1.92 MB  •  keep powered"    (12 pt)
//   y 160  [ Update now ] pill button
//   y 202  control hint                    (12 pt, 148 px band)
lv_obj_t* buildOtaScreen() {
  otaScr = lv_obj_create(nullptr);
  uiThemeScreen(otaScr);

  lv_obj_t* chip = lv_obj_create(otaScr);
  lv_obj_set_size(chip, 36, 36);
  lv_obj_align(chip, LV_ALIGN_TOP_MID, 0, 40);
  lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(chip, UI_COL_PRIMARY, 0);
  lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(chip, 0, 0);
  lv_obj_set_style_pad_all(chip, 0, 0);
  lv_obj_set_scrollable(chip, false);
  lv_obj_t* glyph = uiThemeLabel(chip, &lv_font_montserrat_16, UI_COL_ON_PRIMARY);
  lv_label_set_text(glyph, LV_SYMBOL_DOWNLOAD);
  lv_obj_center(glyph);

  lv_obj_t* title = uiThemeLabel(otaScr, &lv_font_montserrat_16, UI_COL_TEXT);
  lv_label_set_text(title, "Update available");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 84);

  versionLabel = uiThemeLabel(otaScr, &lv_font_montserrat_20, UI_COL_PRIMARY);
  uiThemeTextBounds(versionLabel, 180, 24);
  lv_obj_set_style_text_align(versionLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(versionLabel, LV_ALIGN_TOP_MID, 0, 106);

  sizeLabel = uiThemeLabel(otaScr, &lv_font_montserrat_12, UI_COL_MUTED);
  uiThemeTextBounds(sizeLabel, 180, 16);
  lv_obj_set_style_text_align(sizeLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(sizeLabel, LV_ALIGN_TOP_MID, 0, 134);

  updateBtn = lv_obj_create(otaScr);
  lv_obj_set_size(updateBtn, 140, 36);
  lv_obj_align(updateBtn, LV_ALIGN_TOP_MID, 0, 158);
  uiThemeButton(updateBtn, true);
  lv_obj_set_style_bg_opa(updateBtn, LV_OPA_COVER, 0);
  lv_obj_t* btnLabel = uiThemeLabel(updateBtn, &lv_font_montserrat_14, UI_COL_ON_PRIMARY);
  lv_label_set_text(btnLabel, "Update now");
  lv_obj_center(btnLabel);

  uiThemeControlHint(otaScr, "update");

  return otaScr;
}

} // namespace

void uiScreenOtaShow(const char* newVersion, uint32_t size) {
  // Entered from the menu: free its widgets on the way out (auto_del), the
  // same discipline the create wheel applies to the portfolio list.
  bool freeMenu = g_ui.menuScreen && lv_screen_active() == g_ui.menuScreen;
  if (!otaScr) g_ui.otaScreen = buildOtaScreen();   // lazy build

  char buf[48];
  snprintf(buf, sizeof(buf), "v%s  " LV_SYMBOL_RIGHT "  v%s", FW_VERSION, newVersion);
  lv_label_set_text(versionLabel, buf);
  snprintf(buf, sizeof(buf), "%.2f MB  " LV_SYMBOL_BULLET "  keep powered",
           static_cast<double>(size) / 1048576.0);
  lv_label_set_text(sizeLabel, buf);

  lv_screen_load_anim(otaScr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, freeMenu);
  if (freeMenu) uiScreenMenuFreed();
}

void uiScreenOtaFreed() {
  // The screen object was deleted via the exit transition's auto_del; only
  // the pointers need clearing.
  if (updateBtn) lv_anim_delete(updateBtn, nullptr);
  otaScr = nullptr;
  versionLabel = sizeLabel = updateBtn = nullptr;
  g_ui.otaScreen = nullptr;
}

bool uiScreenOtaActive() {
  return otaScr && lv_screen_active() == otaScr;
}

void uiScreenOtaConfirm() {
  if (!uiScreenOtaActive()) return;
  uiThemePress(updateBtn);
  // main.cpp schedules the update in NVS and reboots into OTA mode a
  // moment later (the loop flushes the "rebooting" overlay first).
  if (g_ui.actions.otaConfirm) g_ui.actions.otaConfirm();
}

void uiScreenOtaCancel() {
  if (!otaScr) return;
  if (g_ui.actions.otaCancel) g_ui.actions.otaCancel();   // BLE ota:status:idle
  uiScreenMenuShow();   // rebuilds the menu; it frees this screen (auto_del)
}
