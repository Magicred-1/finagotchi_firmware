/*
  screen_connect.cpp — app download + Wi-Fi provisioning status.

  The fixed iOS/Android links are pre-generated RGB565 images, so opening
  this screen needs no QR canvas or encoder allocation. Crown rotation switches
  platform, long-press cancels, and the bottom row reports the existing BLE
  provisioning flow without ever displaying credentials.
*/

#include "ui_internal.h"
#include "qr_assets.h"

namespace {

lv_obj_t* connScr;
lv_obj_t* qr;
lv_obj_t* chipIos;
lv_obj_t* chipAndroid;
lv_obj_t* statusLabel;
bool tabIos = true;
char statusText[32] = "waiting for app";

lv_obj_t* chipMake(lv_obj_t* parent, const char* text) {
  lv_obj_t* chip = lv_obj_create(parent);
  lv_obj_set_size(chip, 78, 26);
  uiThemeButton(chip);
  lv_obj_set_style_border_width(chip, 0, 0);
  lv_obj_t* l = uiThemeLabel(chip, &lv_font_montserrat_12, UI_COL_MUTED);
  lv_label_set_text(l, text);
  lv_obj_center(l);
  return chip;
}

void styleChip(lv_obj_t* chip, bool on) {
  lv_obj_t* label = lv_obj_get_child(chip, 0);
  lv_obj_set_style_bg_color(chip, lv_color_hex(0xF6FAFF), 0);
  lv_obj_set_style_bg_opa(chip, on ? LV_OPA_COVER : LV_OPA_0, 0);
  lv_obj_set_style_text_color(label, lv_color_hex(on ? 0x0B2858 : 0x8FA8C9), 0);
}

void styleTabs() {
  styleChip(chipIos, tabIos);
  styleChip(chipAndroid, !tabIos);
}

lv_obj_t* buildConnectScreen() {
  connScr = lv_obj_create(nullptr);
  uiThemeScreen(connScr);
  tabIos = true;

  lv_obj_t* title = uiThemeLabel(connScr, &lv_font_montserrat_16, UI_COL_TEXT);
  lv_label_set_text(title, "get the app");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

  chipIos = chipMake(connScr, "iOS");
  chipAndroid = chipMake(connScr, "Android");
  lv_obj_align(chipIos, LV_ALIGN_TOP_MID, -41, 36);
  lv_obj_align(chipAndroid, LV_ALIGN_TOP_MID, 41, 36);
  styleTabs();

  qr = lv_image_create(connScr);
  lv_image_set_src(qr, &qr_ios);
  lv_image_set_scale(qr, 228);   // 144 -> 128 px: quiet zone stays on the round panel
  lv_obj_align(qr, LV_ALIGN_TOP_MID, 0, 62);

  // Bottom band of the circle: the chord at y ~200 is ~178 px, so the
  // status stays within 160 px (longest: "connected - share Wi-Fi").
  statusLabel = uiThemeLabel(connScr, &lv_font_montserrat_12, UI_COL_INK);
  uiThemeTextBounds(statusLabel, 160, 16);
  lv_obj_set_style_text_align(statusLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(statusLabel, statusText);
  lv_obj_align(statusLabel, LV_ALIGN_BOTTOM_MID, 0, -26);

  return connScr;
}

} // namespace

void uiScreenConnectShow() {
  if (uiScreenConnectActive()) return;
  bool freeMenu = g_ui.menuScreen && lv_screen_active() == g_ui.menuScreen;
  if (freeMenu) uiScreenMenuShrink();
  if (!connScr) g_ui.connectScreen = buildConnectScreen();
  Serial0.printf("UI: Wi-Fi screen heap free=%u min=%u max_block=%u\n",
                 ESP.getFreeHeap(), ESP.getMinFreeHeap(), ESP.getMaxAllocHeap());
  // Fade transitions need a large temporary opacity layer on ESP32-S3;
  // when that allocation fails the old menu shell stays visible and the QR
  // never appears. A transform-only slide needs no extra framebuffer.
  lv_screen_load_anim(connScr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, freeMenu);
  if (freeMenu) uiScreenMenuFreed();
}

void uiScreenConnectToggle() {
  if (!connScr) return;
  tabIos = !tabIos;
  lv_image_set_src(qr, tabIos ? &qr_ios : &qr_android);
  styleTabs();
  uiThemePress(tabIos ? chipIos : chipAndroid);
  uiThemeNudge(qr, -3);
}

void uiScreenConnectSetStatus(const char* text) {
  strlcpy(statusText, text, sizeof(statusText));
  if (statusLabel) lv_label_set_text(statusLabel, statusText);
}

void uiScreenConnectFreed() {
  if (chipIos) lv_anim_delete(chipIos, nullptr);
  if (chipAndroid) lv_anim_delete(chipAndroid, nullptr);
  if (qr) lv_anim_delete(qr, nullptr);
  connScr = nullptr;
  qr = nullptr;
  statusLabel = nullptr;
  chipIos = chipAndroid = nullptr;
  g_ui.connectScreen = nullptr;
}

bool uiScreenConnectActive() {
  return connScr && lv_screen_active() == connScr;
}
