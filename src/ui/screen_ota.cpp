/*
  screen_ota.cpp — firmware update confirm screen.

  Shown when the manifest says a newer firmware exists (menu "Update" row or
  the BLE ota:start command; the check itself lives in main.cpp). Static
  content only — there is nothing to navigate, so both input schemes drive
  it through the existing screen plumbing with no new input code:

    crown push / long press  = confirm (actions.otaConfirm — main.cpp stashes
                               the manifest in NVS and reboots into OTA mode)
    cancel / BTN2 double     = abort (actions.otaCancel, back to the menu)

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

lv_obj_t* buildOtaScreen() {
  otaScr = lv_obj_create(nullptr);
  uiThemeScreen(otaScr);

  lv_obj_t* title = uiThemeLabel(otaScr, &lv_font_montserrat_16, UI_COL_TEXT);
  lv_label_set_text(title, "firmware update");
  lv_obj_align(title, LV_ALIGN_CENTER, 0, -56);

  versionLabel = uiThemeLabel(otaScr, &lv_font_montserrat_28, UI_COL_PRIMARY);
  lv_obj_align(versionLabel, LV_ALIGN_CENTER, 0, -16);

  sizeLabel = uiThemeLabel(otaScr, &lv_font_montserrat_12, UI_COL_MUTED);
  lv_obj_align(sizeLabel, LV_ALIGN_CENTER, 0, 16);

  lv_obj_t* hint = uiThemeLabel(otaScr, &lv_font_montserrat_12, UI_COL_EYEBROW);
  lv_label_set_text(hint, "push = update\ncancel = abort");
  lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(hint, LV_ALIGN_CENTER, 0, 56);

  return otaScr;
}

} // namespace

void uiScreenOtaShow(const char* newVersion, uint32_t size) {
  // Entered from the menu: free its widgets on the way out (auto_del), the
  // same discipline the create wheel applies to the portfolio list.
  bool freeMenu = g_ui.menuScreen && lv_screen_active() == g_ui.menuScreen;
  if (!otaScr) g_ui.otaScreen = buildOtaScreen();   // lazy build

  char buf[40];
  snprintf(buf, sizeof(buf), "v%s -> v%s", FW_VERSION, newVersion);
  lv_label_set_text(versionLabel, buf);
  snprintf(buf, sizeof(buf), "%.2f MB", static_cast<double>(size) / 1048576.0);
  lv_label_set_text(sizeLabel, buf);

  lv_screen_load_anim(otaScr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, freeMenu);
  if (freeMenu) uiScreenMenuFreed();
}

void uiScreenOtaFreed() {
  // The screen object was deleted via the exit transition's auto_del; only
  // the pointers need clearing.
  otaScr = nullptr;
  g_ui.otaScreen = nullptr;
}

bool uiScreenOtaActive() {
  return otaScr && lv_screen_active() == otaScr;
}

void uiScreenOtaConfirm() {
  if (!uiScreenOtaActive()) return;
  // Never returns: main.cpp schedules the update in NVS and reboots into
  // OTA mode. The confirm screen's widgets die with the reboot.
  if (g_ui.actions.otaConfirm) g_ui.actions.otaConfirm();
}

void uiScreenOtaCancel() {
  if (!otaScr) return;
  if (g_ui.actions.otaCancel) g_ui.actions.otaCancel();   // BLE ota:status:idle
  uiScreenMenuShow();   // rebuilds the menu; it frees this screen (auto_del)
}
