/*
  input.cpp — hardware buttons -> LVGL keypad indev + semantic actions.

  Debounce/short/double/long detection is the proven logic from the old
  main.cpp (50 ms debounce, 1000 ms long, 500 ms double window).

  Button mapping (three screens: pet -> portfolio -> menu -> pet)
  ---------------------------------------------------------------
  BTN1 = action (GPIO4, left):
    pet screen:       short  = sync now (ui::Actions.syncNow)
                      double = cycle pet reaction (ui::Actions.cycleReaction)
                      long   = (no-op; mood cycling lives in the menu screen)
    portfolio screen: short  = open the focused card's detail view
                      double = toggle amounts SOL <-> USD
                      long   = back to the pet screen
    detail view:      short  = close the detail view
                      long   = back to the pet screen
    menu screen:      short  = run the focused row (feed / accessory / mood /
                      open DCA — see ui::Actions)
                      long   = back to the pet screen
  BTN2 = navigate (GPIO37, right):
    pet screen:       short  = portfolio screen
                      double = advance the next-buy chip (manual rotate)
    portfolio screen: short  = focus next card; after the last card: menu
                      long   = back to the pet screen
    detail view:      short  = close the detail view
    menu screen:      short  = focus next row; after the last row: pet screen
    any screen:       long   = ALWAYS back to the pet screen

  The focus group only ever holds the visible objects of the ACTIVE screen
  (see uiGroupSet) — hidden cards never swallow key presses. Card/row
  activation goes through LV_KEY_ENTER on the keypad indev so focus visuals
  come from the framework.
*/

#include "ui_internal.h"

namespace {

constexpr uint8_t BUTTON_1_PIN = 4;    // left button
constexpr uint8_t BUTTON_2_PIN = 37;   // right button

constexpr uint32_t DEBOUNCE_MS    = 50;
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
  } else if (act == g_ui.petScreen) {
    uiScreenDcaShow();
    Serial.println("BTN2: portfolio screen");
  } else if (act == g_ui.dcaScreen) {
    if (!uiScreenDcaFocusAdvance()) {
      uiScreenMenuShow();
      Serial.println("BTN2: menu screen");
    }
  } else if (act == g_ui.menuScreen) {
    if (!uiScreenMenuFocusAdvance()) {
      uiScreenPetShow();
      Serial.println("BTN2: pet screen");
    }
  }
}

void onBtn2Double() {
  if (lv_screen_active() == g_ui.petScreen) {
    uiScreenPetChipAdvance();
    Serial.println("BTN2 double: next-buy chip advance");
  }
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
  bool long1, long2;
  bool short1 = handleButton(btn1, long1);
  bool short2 = handleButton(btn2, long2);

  // BTN1 short/double (500 ms window: a second short within it = double).
  static uint32_t lastShort1 = 0;
  if (short1) {
    if (millis() - lastShort1 < DOUBLE_PRESS_MS) {
      lastShort1 = 0;
      onBtn1Double(nowSec);
    } else {
      lastShort1 = millis();
      onBtn1Short(nowSec);
    }
  }

  static uint32_t lastShort2 = 0;
  if (short2) {
    if (millis() - lastShort2 < DOUBLE_PRESS_MS) {
      lastShort2 = 0;
      onBtn2Double();
    } else {
      lastShort2 = millis();
      onBtn2Short();
    }
  }

  if (long1) onBtn1Long();

  // BTN2 long: ALWAYS back to the pet screen.
  if (long2) {
    uiScreenPetShow();
    Serial.println("BTN2 long: pet screen");
  }
}
