/*
  screen_lock.cpp — parental code pad (optional 4-digit code).

  When main.cpp has a code stored (NVS), the device boots LOCKED: the pet
  and every cosmetic screen stay free, but the FINANCIAL scope — the
  portfolio screen, the create-plan wheel behind it, and the Portfolio
  menu row — lands here first. While locked the screen cycle
  skips the portfolio entirely (pet -> menu -> pet). One good code
  unlocks the session (until reboot); back / crown long-push cancels the pad.
  From the menu's "Code lock" row: no code set -> set one (enter twice);
  code set -> enter it to remove. Boot recovery clears a forgotten code too.

  Input rides the same buttons/crown as everything else (the focus group is
  left untouched — digits are driven directly):
    short / CW  = increment the current digit (0-9, wraps)
    CCW         = decrement (PCB v1)
    long / push = confirm digit, move on; after the 4th, validate

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
lv_obj_t* waitLabel;     // "try again in 30s" during a wrong-code lockout
lv_timer_t* waitTimer;
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

// Wrong-code lockout (main.cpp owns the counter + NVS): only the modes that
// VALIDATE a stored code are throttled; setting a new code is not.
uint32_t lockoutMs() {
  if (mode != MODE_UNLOCK && mode != MODE_REMOVE) return 0;
  return g_ui.actions.pinLockoutMs ? g_ui.actions.pinLockoutMs() : 0;
}

void waitRefresh(lv_timer_t* = nullptr) {
  if (!waitLabel) return;
  uint32_t ms = lockoutMs();
  if (ms == 0) {
    lv_obj_set_hidden(waitLabel, true);
    for (uint8_t i = 0; i < 4; i++) lv_obj_set_style_opa(digit[i], LV_OPA_COVER, 0);
    return;
  }
  char buf[28];
  uint32_t s = (ms + 999) / 1000;
  if (s >= 60) snprintf(buf, sizeof(buf), "try again in %lum %02lus",
                        static_cast<unsigned long>(s / 60), static_cast<unsigned long>(s % 60));
  else snprintf(buf, sizeof(buf), "try again in %lus", static_cast<unsigned long>(s));
  lv_label_set_text(waitLabel, buf);
  lv_obj_set_hidden(waitLabel, false);
  for (uint8_t i = 0; i < 4; i++) lv_obj_set_style_opa(digit[i], LV_OPA_40, 0);
}

void wrongCode() {
  uiScreenPetToast("wrong code", false);
  showMode(mode, mode == MODE_REMOVE ? "remove code" : "enter code");
  waitRefresh();
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

  lv_obj_t* glyph = uiThemeLabel(lockScr, &lv_font_montserrat_20, UI_COL_PRIMARY);
  lv_label_set_text(glyph, LV_SYMBOL_EYE_CLOSE);
  lv_obj_align(glyph, LV_ALIGN_CENTER, 0, -86);

  titleLabel = uiThemeLabel(lockScr, &lv_font_montserrat_16, UI_COL_TEXT);
  lv_obj_align(titleLabel, LV_ALIGN_CENTER, 0, -56);

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

  waitLabel = uiThemeLabel(lockScr, &lv_font_montserrat_12, UI_COL_WARNING);
  uiThemeTextBounds(waitLabel, 180, 16);
  lv_obj_set_style_text_align(waitLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(waitLabel, LV_ALIGN_CENTER, 0, 46);
  lv_obj_set_hidden(waitLabel, true);
  waitTimer = lv_timer_create(waitRefresh, 1000, nullptr);

  uiThemeControlHint(lockScr, "next");

  return lockScr;
}

} // namespace

void uiScreenLockShow(int m) {
  if (!lockScr) g_ui.lockScreen = buildLockScreen();   // lazy build
  const char* title = m == 1 ? "set code" : m == 3 ? "remove code" : "enter code";
  showMode(static_cast<uint8_t>(m == 1 || m == 3 || m == 0 ? m : 0), title);
  waitRefresh();
  lv_screen_load_anim(lockScr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 160, 0, false);
}

void uiScreenLockFreed() {
  for (uint8_t i = 0; i < 4; i++) lv_anim_delete(digit[i], nullptr);
  if (waitTimer) { lv_timer_delete(waitTimer); waitTimer = nullptr; }
  waitLabel = nullptr;
  lockScr = nullptr;
  g_ui.lockScreen = nullptr;
}

bool uiScreenLockActive() {
  return lockScr && lv_screen_active() == lockScr;
}

void uiScreenLockNext() {
  if (!lockScr || lockoutMs() > 0) return;
  digits[pos] = (digits[pos] + 1) % 10;
  digitsDraw();
  uiThemeNudge(digit[pos], -5);
}

// Crown CCW (PCB v1): step the current digit down, wrapping 0 -> 9.
void uiScreenLockPrev() {
  if (!lockScr || lockoutMs() > 0) return;
  digits[pos] = (digits[pos] + 9) % 10;
  digitsDraw();
  uiThemeNudge(digit[pos], 5);
}

void uiScreenLockConfirm() {
  if (!lockScr) return;
  if (lockoutMs() > 0) { waitRefresh(); uiThemeNudge(waitLabel, -3); return; }
  uint8_t confirmed = pos;
  if (++pos >= 4) complete();
  else {
    digitsDraw();
    uiThemeFocusPop(digit[confirmed]);
  }
}
