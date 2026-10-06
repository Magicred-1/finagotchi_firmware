/*
  input.cpp — hardware buttons -> LVGL keypad indev + semantic actions.

  Debounce/short/double/long detection is the proven logic from the old
  main.cpp (30 ms debounce, 1000 ms long, 500 ms double window).

  FULLY REDUNDANT TWO-BUTTON SCHEME. Serial captures on this hardware
  have shown EACH button electrically silent at different times (first
  GPIO4 dead / GPIO37 alive, later the reverse) — flaky unit. So both
  buttons carry the SAME mapping and the UI works with either one:

    ANY button, any screen:
      short = navigate forward (pet -> portfolio -> menu -> pet;
              walks card/row focus within portfolio/menu first;
              on the create screen: next field; on a detail view: close,
              deferred by the double window — see onDetailShort)
      long  = action for the current screen:
              pet = sync now, portfolio = open the focused card,
              detail = pause/resume the plan, menu = run focused action,
              create = cycle the focused field's value / send

  Bonus (left button only, when alive): double-press on the pet screen
  cycles the pet reaction, on the portfolio toggles SOL <-> USD amounts,
  on a detail view double-press also toggles pause (either button).

  The focus group only ever holds the visible objects of the ACTIVE screen
  (see uiGroupSet) — hidden cards never swallow key presses. Card/row
  activation goes through LV_KEY_ENTER on the keypad indev so focus visuals
  come from the framework.
*/

#include "ui_internal.h"

namespace {

// Hardware reality: captures have shown each button electrically dead at
// different times (flaky unit), so BOTH buttons carry the same mapping —
// short = navigate, long = action — and the UI works with either one.
constexpr uint8_t BUTTON_1_PIN = 4;    // left button
constexpr uint8_t BUTTON_2_PIN = 37;   // right button

constexpr uint32_t DEBOUNCE_MS    = 30;
constexpr uint32_t LONG_PRESS_MS  = 1000;
constexpr uint32_t DOUBLE_PRESS_MS = 500;

struct Button {
  uint8_t  pin;
  bool     lastRaw;
  bool     state;      // debounced state (LOW = pressed)
  uint32_t lastChange;
  uint32_t pressedAt;
  bool     longFired;
};

Button btn1 = { BUTTON_1_PIN, HIGH, HIGH, 0, 0, false };
Button btn2 = { BUTTON_2_PIN, HIGH, HIGH, 0, 0, false };

// Returns true on short-press release, sets longFired on long press.
bool handleButton(Button& b, bool& longPress) {
  bool raw = digitalRead(b.pin);
  uint32_t now = millis();

  if (raw != b.lastRaw) {
    b.lastChange = now;
    b.lastRaw = raw;
  }

  longPress = false;

  if ((now - b.lastChange) > DEBOUNCE_MS && raw != b.state) {
    b.state = raw;
    if (b.state == LOW) {           // pressed
      b.pressedAt = now;
      b.longFired = false;
    } else {                        // released
      if (!b.longFired && (now - b.pressedAt) < LONG_PRESS_MS) {
        return true;                // short press
      }
    }
  }

  if (b.state == LOW && !b.longFired && (now - b.pressedAt) >= LONG_PRESS_MS) {
    b.longFired = true;
    longPress = true;               // long press detected
  }

  return false;
}

// --- keypad indev key queue -------------------------------------------------

uint32_t keyBuf[4];
uint8_t  keyHead = 0, keyTail = 0;

void pushKey(uint32_t key) {
  if (static_cast<uint8_t>(keyHead - keyTail) < 4)
    keyBuf[keyHead++ % 4] = key;
}

void keypadRead(lv_indev_t*, lv_indev_data_t* data) {
  static uint32_t held = 0;
  if (held) {   // every queued key is delivered pressed, then released
    data->key = held;
    data->state = LV_INDEV_STATE_RELEASED;
    held = 0;
    data->continue_reading = keyHead != keyTail;
    return;
  }
  if (keyHead == keyTail) {
    data->state = LV_INDEV_STATE_RELEASED;
    data->continue_reading = false;
    return;
  }
  held = keyBuf[keyTail++ % 4];
  data->key = held;
  data->state = LV_INDEV_STATE_PRESSED;
  data->continue_reading = true;
}

// --- semantic actions --------------------------------------------------------

// Detail-screen double-press: toggles pause/resume on either button. A
// plain short press still closes the detail, but only after the double
// window proves it wasn't a toggle — the close is deferred by up to
// DOUBLE_PRESS_MS (the detail view is the one screen where a press that
// would navigate away also has a double action, so it can't fire the
// short immediately like the pet/portfolio doubles do).
uint32_t detailShortAt = 0;   // pending close; 0 = none

void onDetailShort() {
  if (detailShortAt && millis() - detailShortAt < DOUBLE_PRESS_MS) {
    detailShortAt = 0;
    uiScreenDcaTogglePause();
    Serial.println("detail double: pause toggle");
  } else {
    detailShortAt = millis();
  }
}

// Short press (either button) = navigate forward. Screens cycle, so the
// pet screen is always a few presses away.
void onNavShort() {
  lv_obj_t* act = lv_screen_active();
  if (uiScreenDcaDetailOpen()) {
    onDetailShort();
  } else if (act == g_ui.petScreen) {
    uiScreenDcaShow();
    Serial.println("NAV: portfolio screen");
  } else if (act == g_ui.dcaScreen) {
    if (uiScreenDcaFocusAdvance()) {
      Serial.println("NAV: focus next card");
    } else {
      uiScreenMenuShow();
      Serial.println("NAV: menu screen");
    }
  } else if (act == g_ui.createScreen) {
    uiScreenCreateFocusAdvance();
    Serial.println("NAV: create: next field");
  } else if (act == g_ui.menuScreen) {
    if (uiScreenMenuFocusAdvance()) {
      Serial.println("NAV: focus next row");
    } else {
      uiScreenPetShow();
      Serial.println("NAV: pet screen");
    }
  }
}

// Long press (either button) = the action of the current screen.
void onActionLong(float nowSec) {
  lv_obj_t* act = lv_screen_active();
  Serial.println("ACTION (long)");
  if (uiScreenDcaDetailOpen()) {
    uiScreenDcaTogglePause();
  } else if (act == g_ui.createScreen) {
    uiScreenCreateActivate(nowSec);
  } else if (act == g_ui.menuScreen) {
    uiScreenMenuActivate(nowSec);
  } else if (act == g_ui.dcaScreen) {
    pushKey(LV_KEY_ENTER);   // open the focused card (safe no-op when empty)
  } else if (g_ui.actions.syncNow) {
    g_ui.actions.syncNow(nowSec);
  }
}

// Bonus: left-button double-press extras (when the left button is alive).
void onBtn1Double(float nowSec) {
  lv_obj_t* act = lv_screen_active();
  if (act == g_ui.dcaScreen && !uiScreenDcaDetailOpen()) {
    uiScreenDcaToggleAmountUnit();
    Serial.printf("BTN1 double: amounts in %s\n", g_ui.amountInSol ? "SOL" : "USD");
  } else if (act == g_ui.petScreen && g_ui.actions.cycleReaction) {
    g_ui.actions.cycleReaction(nowSec);
  }
}

// Double-press only eats the second press where a double action actually
// exists on the current screen — everywhere else every press is a short,
// so fast tapping never swallows navigation.
bool btn1HasDoubleHere() {
  lv_obj_t* act = lv_screen_active();
  if (act == g_ui.petScreen) return true;                       // reaction
  if (act == g_ui.dcaScreen && !uiScreenDcaDetailOpen()) return true;  // USD/SOL
  return false;
}

} // namespace

void uiInputInit() {
  pinMode(btn1.pin, INPUT_PULLUP);
  pinMode(btn2.pin, INPUT_PULLUP);

  lv_indev_t* indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_KEYPAD);
  lv_indev_set_read_cb(indev, keypadRead);
  lv_indev_set_group(indev, g_ui.group);

  Serial.printf("Buttons: GPIO%d + GPIO%d (short = navigate, long = action)\n",
                btn1.pin, btn2.pin);
}

void uiInputUpdate() {
  float nowSec = millis() / 1000.0f;

  bool long1, long2;
  bool short1 = handleButton(btn1, long1);
  bool short2 = handleButton(btn2, long2);

  // Deferred detail close: the double window expired without a second
  // press, so it was a plain short — close the detail (if still open).
  if (detailShortAt && millis() - detailShortAt >= DOUBLE_PRESS_MS) {
    detailShortAt = 0;
    if (uiScreenDcaDetailOpen()) {
      uiScreenDcaCloseDetail();
      Serial.println("detail: close");
    }
  }

  // BTN1 double extras: the 500 ms double window only applies where the
  // screen has a double action (btn1HasDoubleHere) — otherwise every press
  // is a short, so mashing never eats presses.
  static uint32_t lastShort1 = 0;
  if (short1) {
    if (btn1HasDoubleHere() && millis() - lastShort1 < DOUBLE_PRESS_MS) {
      lastShort1 = 0;
      onBtn1Double(nowSec);
    } else {
      lastShort1 = millis();
      onNavShort();
    }
  }

  // Both buttons navigate on short, act on long — fully redundant.
  if (short2) onNavShort();

  if (long1) onActionLong(nowSec);

  if (long2) onActionLong(nowSec);
}
