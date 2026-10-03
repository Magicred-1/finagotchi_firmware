/*
  input.cpp — hardware buttons -> LVGL keypad indev + semantic actions.

  Debounce/short/double/long detection is the proven logic from the old
  main.cpp (50 ms debounce, 1000 ms long, 500 ms double window).

  Button mapping
  --------------
  BTN1 = action (GPIO4, left):
    pet screen:   short  = sync now (ui::Actions.syncNow)
                  double = cycle pet reaction (ui::Actions.cycleReaction)
                  long   = cycle pet mood (ui::Actions.cycleMood)
    DCA screen:   short  = activate focused card (detail view) / close detail
                  double = toggle card amounts SOL <-> USD
                  long   = cycle pet mood (ui::Actions.cycleMood)
  BTN2 = navigate (GPIO37, right):
    pet screen:   short  = go to the DCA screen
                  double = advance the next-buy chip (manual rotate)
                  long   = (no-op)
    DCA screen:   short  = focus next card / close detail
                  double = (no-op)
                  long   = back to the pet screen

  Focus navigation and card activation go through a LV_INDEV_TYPE_KEYPAD
  bound to the card focus group, so focus visuals come from the framework;
  everything else calls the semantic handlers directly.
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

// True on the DCA list screen and on the card detail screen (the pet screen
// is the only non-DCA screen).
bool dcaActive() { return lv_screen_active() != g_ui.petScreen; }

void onBtn1Short(float nowSec) {
  if (dcaActive()) {
    if (uiScreenDcaDetailOpen()) uiScreenDcaCloseDetail();
    else pushKey(LV_KEY_ENTER);   // activate the focused card
  } else if (g_ui.actions.syncNow) {
    g_ui.actions.syncNow(nowSec);
  }
}

void onBtn1Double(float nowSec) {
  if (dcaActive()) {
    uiScreenDcaToggleAmountUnit();
    Serial.printf("BTN1 double: amounts in %s\n", g_ui.amountInSol ? "SOL" : "USD");
  } else if (g_ui.actions.cycleReaction) {
    g_ui.actions.cycleReaction(nowSec);
  }
}

void onBtn2Short() {
  if (dcaActive()) {
    if (uiScreenDcaDetailOpen()) uiScreenDcaCloseDetail();
    else pushKey(LV_KEY_NEXT);    // focus the next card
  } else {
    uiScreenDcaShow();
    Serial.println("BTN2: DCA screen");
  }
}

void onBtn2Double() {
  if (!dcaActive()) {
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

  // BTN1 long: cycle mood (either screen; the pet scene is behind both).
  if (long1 && g_ui.actions.cycleMood) {
    g_ui.actions.cycleMood(nowSec);
    Serial.println("BTN1 long: mood cycle");
  }

  // BTN2 long: back to the pet screen.
  if (long2) {
    uiScreenPetShow();
    Serial.println("BTN2 long: pet screen");
  }
}
