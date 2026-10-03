/*
  input.cpp — hardware buttons -> LVGL keypad indev + semantic actions.

  Debounce/short/double/long detection is the proven logic from the old
  main.cpp (30 ms debounce, 1000 ms long, 500 ms double window).

  SINGLE-BUTTON SCHEME (the left button is dead on this unit — serial
  captures showed GPIO37 presses only). The RIGHT button (BTN2, GPIO37)
  carries the whole UI; the LEFT button (BTN1, GPIO4) is a bonus action
  key if it ever comes back.

  BTN2 = right (GPIO37) — primary:
    any screen:    short = navigate forward (pet -> portfolio -> menu -> pet;
                   walks card/row focus within portfolio/menu first)
                   long  = action (same as BTN1 short: select / activate)
  BTN1 = left (GPIO4) — bonus action key:
    pet screen:       short  = sync now, double = cycle pet reaction
    portfolio screen: short  = open detail, double = toggle SOL <-> USD
    detail view:      short  = close the detail view
    menu screen:      short  = run the focused action
    any non-pet:      long   = back to the pet screen

  The focus group only ever holds the visible objects of the ACTIVE screen
  (see uiGroupSet) — hidden cards never swallow key presses. Card/row
  activation goes through LV_KEY_ENTER on the keypad indev so focus visuals
  come from the framework.
*/

#include "ui_internal.h"

namespace {

// Hardware reality: only the RIGHT button (GPIO37) is electrically alive —
// the left (GPIO4) never showed a single transition in serial captures.
// So the right button carries the whole UI (single-button scheme):
//   short = navigate (next), long = action (select), and screens cycle so
//   "home" is always a few presses away.
// The left button stays mapped as a bonus action key if it ever comes back.
constexpr uint8_t BUTTON_1_PIN = 4;    // left button — action (bonus; dead on some units)
constexpr uint8_t BUTTON_2_PIN = 37;   // right button — navigate (short) / action (long)

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

void onBtn1Short(float nowSec) {
  lv_obj_t* act = lv_screen_active();
  if (uiScreenDcaDetailOpen()) {
    uiScreenDcaCloseDetail();
  } else if (act == g_ui.menuScreen) {
    uiScreenMenuActivate(nowSec);
  } else if (act == g_ui.dcaScreen) {
    pushKey(LV_KEY_ENTER);   // open the focused card (safe no-op when empty)
  } else if (g_ui.actions.syncNow) {
    g_ui.actions.syncNow(nowSec);
  }
}

void onBtn1Double(float nowSec) {
  lv_obj_t* act = lv_screen_active();
  if (act == g_ui.dcaScreen && !uiScreenDcaDetailOpen()) {
    uiScreenDcaToggleAmountUnit();
    Serial.printf("BTN1 double: amounts in %s\n", g_ui.amountInSol ? "SOL" : "USD");
  } else if (act == g_ui.petScreen && g_ui.actions.cycleReaction) {
    g_ui.actions.cycleReaction(nowSec);
  }
}

void onBtn1Long() {
  if (lv_screen_active() != g_ui.petScreen) {
    uiScreenPetShow();
    Serial.println("BTN1 long: pet screen");
  }
  // On the pet screen itself BTN1 long is a no-op — mood cycling lives in
  // the menu screen (see screen_menu.cpp).
}

void onBtn2Short() {
  lv_obj_t* act = lv_screen_active();
  if (uiScreenDcaDetailOpen()) {
    uiScreenDcaCloseDetail();
    Serial.println("BTN2: close detail");
  } else if (act == g_ui.petScreen) {
    uiScreenDcaShow();
    Serial.println("BTN2: portfolio screen");
  } else if (act == g_ui.dcaScreen) {
    if (uiScreenDcaFocusAdvance()) {
      Serial.println("BTN2: focus next card");
    } else {
      uiScreenMenuShow();
      Serial.println("BTN2: menu screen");
    }
  } else if (act == g_ui.menuScreen) {
    if (uiScreenMenuFocusAdvance()) {
      Serial.println("BTN2: focus next row");
    } else {
      uiScreenPetShow();
      Serial.println("BTN2: pet screen");
    }
  }
}

// Double-press only eats the second press where a double action actually
// exists on the current screen — everywhere else every press is a short,
// so fast tapping never swallows navigation. (Only BTN1/left has doubles;
// the right button is navigation, every press counts.)
bool btn1HasDoubleHere() {
  lv_obj_t* act = lv_screen_active();
  if (act == g_ui.petScreen) return true;                       // reaction
  if (act == g_ui.dcaScreen && !uiScreenDcaDetailOpen()) return true;  // USD/SOL
  return false;
}

// Right button LONG = action (single-button scheme: the left button is dead
// on this unit, so long-press carries the select role on the live button).
void onBtn2Long(float nowSec) {
  Serial.println("BTN2 long: action");
  onBtn1Short(nowSec);
}

} // namespace

void uiInputInit() {
  pinMode(btn1.pin, INPUT_PULLUP);
  pinMode(btn2.pin, INPUT_PULLUP);

  lv_indev_t* indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_KEYPAD);
  lv_indev_set_read_cb(indev, keypadRead);
  lv_indev_set_group(indev, g_ui.group);

  Serial.printf("Buttons: GPIO%d (action), GPIO%d (navigate)\n", btn1.pin, btn2.pin);
}

void uiInputUpdate() {
  float nowSec = millis() / 1000.0f;

  // DIAG (temporary): raw GPIO transitions, before debounce — separates
  // "pin never changes" (hardware) from "logic drops the press" (firmware).
  static bool dbgRaw1 = HIGH, dbgRaw2 = HIGH;
  bool r1 = digitalRead(BUTTON_1_PIN);
  bool r2 = digitalRead(BUTTON_2_PIN);
  if (r1 != dbgRaw1) { dbgRaw1 = r1; Serial.printf("DBG BTN1 raw=%d\n", r1); }
  if (r2 != dbgRaw2) { dbgRaw2 = r2; Serial.printf("DBG BTN2 raw=%d\n", r2); }

  bool long1, long2;
  bool short1 = handleButton(btn1, long1);
  bool short2 = handleButton(btn2, long2);

  // BTN1 short/double: the 500 ms double window only applies where the
  // screen has a double action (btn1HasDoubleHere) — otherwise every press
  // is a short, so mashing never eats presses.
  static uint32_t lastShort1 = 0;
  if (short1) {
    if (btn1HasDoubleHere() && millis() - lastShort1 < DOUBLE_PRESS_MS) {
      lastShort1 = 0;
      onBtn1Double(nowSec);
    } else {
      lastShort1 = millis();
      onBtn1Short(nowSec);
    }
  }

  // BTN2 (right, the live button): short = navigate, long = action. No
  // double-press on this button — every press moves you forward.
  if (short2) onBtn2Short();

  if (long1) onBtn1Long();

  if (long2) onBtn2Long(nowSec);
}
