/*
  screen_lock.cpp — parental code pad (optional 4-digit code).

  When main.cpp has a code stored (NVS), the device boots LOCKED: the pet
  and every cosmetic screen stay free, but the FINANCIAL scope — the
  portfolio, the create-plan wheel behind it, and the Portfolio /
  Open DCA menu rows — lands here first. While locked the screen cycle
  skips the portfolio entirely (pet -> menu -> pet). One good code
  unlocks the session (until reboot); a double-press cancels the pad.
  From the menu's "Parental" row: no code set -> set one (enter twice);
  code set -> enter it to remove. Forgotten code? The boot recovery chord
  (hold BOTH buttons while powering on) clears it along with the BLE bonds.

  Input rides the same two buttons as everything else (the focus group is
  left untouched — digits are driven directly):
    short = increment the current digit (0-9, wraps)
    long  = confirm digit, move on; after the 4th, validate

  Lazy build + free on exit (auto_del), same heap discipline as the menu
  and create screens. PIN check/store/clear live in main.cpp
  (ui::Actions::pinCheck/pinStore/pinClear).
*/

#include "ui_internal.h"

namespace {

constexpr uint8_t MODE_UNLOCK = 0;
constexpr uint8_t MODE_SET_NEW = 1;
constexpr uint8_t MODE_SET_CONFIRM = 2;
constexpr uint8_t MODE_REMOVE = 3;

lv_obj_t* lockScr;
lv_obj_t* titleLabel;
lv_obj_t* digit[4];
uint8_t   mode = MODE_UNLOCK;
uint8_t   digits[4] = { 0, 0, 0, 0 };
uint8_t   pos = 0;
uint16_t  stashed = 0;   // first entry during SET_NEW -> SET_CONFIRM

uint16_t digitsAsPin() {
  return static_cast<uint16_t>(digits[0] * 1000 + digits[1] * 100 +
                               digits[2] * 10 + digits[3]);
}

void digitsReset() {
  memset(digits, 0, sizeof(digits));
  pos = 0;
}

void digitsDraw() {
  for (uint8_t i = 0; i < 4; i++) {
    char c[2] = { static_cast<char>('0' + digits[i]), 0 };
    lv_label_set_text(digit[i], c);
    bool cur = (i == pos);
    lv_obj_set_style_border_color(digit[i], cur ? UI_COL_PRIMARY : UI_COL_BORDER, 0);
    lv_obj_set_style_border_opa(digit[i], cur ? LV_OPA_COVER : LV_OPA_30, 0);
    lv_obj_set_style_text_color(digit[i], cur ? UI_COL_TEXT : UI_COL_MUTED, 0);
  }
}

void showMode(uint8_t m, const char* title) {
  mode = m;
  digitsReset();
  lv_label_set_text(titleLabel, title);
  digitsDraw();
}

void wrongCode() {
  uiScreenPetToast("wrong code", false);
  showMode(mode, mode == MODE_REMOVE ? "remove code" : "enter code");
}

// 4th digit confirmed: validate / advance the mode machine.
void complete() {
  uint16_t pin = digitsAsPin();
  switch (mode) {
    case MODE_UNLOCK:
      if (g_ui.actions.pinCheck && g_ui.actions.pinCheck(pin)) {
        g_ui.locked = false;
        uiScreenPetToast("unlocked", false);
        uiScreenPetShow();
      } else {
        wrongCode();
      }
      break;
    case MODE_SET_NEW:
      stashed = pin;
      showMode(MODE_SET_CONFIRM, "confirm code");
      break;
    case MODE_SET_CONFIRM:
      if (pin == stashed) {
        if (g_ui.actions.pinStore) g_ui.actions.pinStore(pin);
        uiScreenPetToast("code set", false);
        uiScreenPetShow();
      } else {
        uiScreenPetToast("no match", false);
        showMode(MODE_SET_NEW, "set code");
      }
      break;
    default:   // MODE_REMOVE
      if (g_ui.actions.pinCheck && g_ui.actions.pinCheck(pin)) {
        if (g_ui.actions.pinClear) g_ui.actions.pinClear();
        uiScreenPetToast("code removed", false);
        uiScreenPetShow();
      } else {
        wrongCode();
      }
      break;
  }
}

lv_obj_t* buildLockScreen() {
  lockScr = lv_obj_create(nullptr);
  uiThemeScreen(lockScr);

  titleLabel = uiThemeLabel(lockScr, &lv_font_montserrat_16, UI_COL_TEXT);
  lv_obj_align(titleLabel, LV_ALIGN_CENTER, 0, -64);

  for (uint8_t i = 0; i < 4; i++) {
    digit[i] = uiThemeLabel(lockScr, &lv_font_montserrat_28, UI_COL_TEXT);
    lv_obj_set_size(digit[i], 40, 52);
    lv_obj_align(digit[i], LV_ALIGN_CENTER, -66 + i * 44, 0);
    lv_obj_set_style_text_align(digit[i], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(digit[i], 8, 0);
    lv_obj_set_style_radius(digit[i], UI_RADIUS_SMALL, 0);
    lv_obj_set_style_bg_color(digit[i], UI_COL_SURFACE, 0);
    lv_obj_set_style_bg_opa(digit[i], LV_OPA_40, 0);
    lv_obj_set_style_border_width(digit[i], 2, 0);
  }

  return lockScr;
}

} // namespace

void uiScreenLockShow(int m) {
  if (!lockScr) g_ui.lockScreen = buildLockScreen();   // lazy build
  const char* title = m == 1 ? "set code" : m == 3 ? "remove code" : "enter code";
  showMode(static_cast<uint8_t>(m == 1 || m == 3 || m == 0 ? m : 0), title);
  lv_screen_load_anim(lockScr, LV_SCR_LOAD_ANIM_FADE_IN, 200, 0, false);
}

void uiScreenLockFreed() {
  // The screen object was deleted via the exit transition's auto_del; only
  // the pointers need clearing.
  lockScr = nullptr;
  g_ui.lockScreen = nullptr;
}

bool uiScreenLockActive() {
  return lockScr && lv_screen_active() == lockScr;
}

void uiScreenLockNext() {
  if (!lockScr) return;
  digits[pos] = (digits[pos] + 1) % 10;
  digitsDraw();
}

void uiScreenLockConfirm() {
  if (!lockScr) return;
  if (++pos >= 4) complete();
  else digitsDraw();
}
