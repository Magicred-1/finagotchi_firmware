/*
  input.cpp — hardware buttons -> LVGL keypad indev + semantic actions.

  Buttons use 30 ms debounce and a 1000 ms long-press threshold. Confirm is
  immediate: no double-press window delays an action or steals navigation.

  TWO-BUTTON SCHEME (breadboard):
    LEFT short = next; RIGHT short = back; either long = confirm/action.
    RIGHT short on the pet screen opens the menu directly.

  PCB v1 needs only the crown; GPIO7 is an optional back alias:
    crown CW/CCW = next/previous, short = confirm, long = back.

  The menu exposes Play and Units explicitly, so extras never depend on a
  hidden double-press gesture. The focus group only holds visible objects of
  the active screen (see uiGroupSet), and activation uses LV_KEY_ENTER so
  focus visuals come from LVGL.
*/

#include "ui_internal.h"
#include "feedback.h"

namespace {

#ifdef FINAGOTCHI_PCB_V1
// PCB v1: crown encoder (quadrature A/B + active-LOW push, 10k pull-ups on
// the board) and a dedicated active-HIGH cancel button (10k pull-down).
constexpr uint8_t CROWN_A_PIN    = 15;
constexpr uint8_t CROWN_B_PIN    = 16;
constexpr uint8_t CROWN_PUSH_PIN = 4;    // same pin/polarity as breadboard BTN1
constexpr uint8_t CANCEL_PIN     = 7;
#else
constexpr uint8_t BUTTON_1_PIN = 4;    // left button
constexpr uint8_t BUTTON_2_PIN = 37;   // right button
#endif

constexpr uint32_t DEBOUNCE_MS    = 30;
constexpr uint32_t LONG_PRESS_MS  = 1000;

struct Button {
  uint8_t  pin;
  uint8_t  activeLevel;  // LOW with pull-ups, HIGH for the PCB cancel button
  bool     lastRaw;      // last raw reading, true = pressed side
  bool     pressed;      // debounced state
  uint32_t lastChange;
  uint32_t pressedAt;
  bool     longFired;
};

#ifdef FINAGOTCHI_PCB_V1
Button btnPush   = { CROWN_PUSH_PIN, LOW,  false, false, 0, 0, false };
Button btnCancel = { CANCEL_PIN,     HIGH, false, false, 0, 0, false };
#else
Button btn1 = { BUTTON_1_PIN, LOW, false, false, 0, 0, false };
Button btn2 = { BUTTON_2_PIN, LOW, false, false, 0, 0, false };
#endif

// Returns true on short-press release, sets longFired on long press.
bool handleButton(Button& b, bool& longPress) {
  bool raw = digitalRead(b.pin) == b.activeLevel;
  uint32_t now = millis();

  if (raw != b.lastRaw) {
    b.lastChange = now;
    b.lastRaw = raw;
  }

  longPress = false;

  if ((now - b.lastChange) > DEBOUNCE_MS && raw != b.pressed) {
    b.pressed = raw;
    if (b.pressed) {                  // pressed
      b.pressedAt = now;
      b.longFired = false;
    } else {                          // released
      if (!b.longFired && (now - b.pressedAt) < LONG_PRESS_MS) {
        return true;                  // short press
      }
    }
  }

  if (b.pressed && !b.longFired && (now - b.pressedAt) >= LONG_PRESS_MS) {
    b.longFired = true;
    longPress = true;                 // long press detected
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

// Next moves through the visible focusables, then through the screen cycle.
void onNavShort() {
  if (uiScreenLockActive()) { uiScreenLockNext(); return; }
  // connect QR prompt: navigate switches the iOS/Android tab
  if (uiScreenConnectActive()) { uiScreenConnectToggle(); return; }
  lv_obj_t* act = lv_screen_active();
  if (uiScreenDcaDetailOpen()) {
    uiScreenDcaCloseDetail();
  } else if (act == g_ui.petScreen) {
    if (g_ui.locked) {
      // Parental gate: the portfolio (financial) is SKIPPED while locked —
      // the cycle becomes pet -> menu -> pet so cosmetics stay reachable.
      // The financial menu rows ask for the code on activation.
      uiScreenMenuShow();
      Serial.println("NAV: menu screen (locked: portfolio skipped)");
    } else {
      uiScreenDcaShow();
      Serial.println("NAV: portfolio screen");
    }
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

// Breadboard long press / PCB short push = the current screen's action.
void onConfirm(float nowSec) {
  if (uiScreenLockActive()) { uiScreenLockConfirm(); return; }
  // OTA confirm screen: push/long = schedule the update + reboot into OTA
  // mode (nothing else on that screen to act on).
  if (uiScreenOtaActive()) { uiScreenOtaConfirm(); return; }
  // Connect QR prompt is static: no action while it's up (only back).
  if (uiScreenConnectActive()) return;
  lv_obj_t* act = lv_screen_active();
  Serial.println("ACTION confirm");
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

// Back cancels the code pad, closes detail, or leaves the current screen.
// The breadboard's pet-screen menu shortcut is handled in uiInputUpdate.
void onBack() {
  if (uiScreenConnectActive()) {
    uiScreenMenuShow();   // QR prompt: back returns to the menu
    Serial.println("BACK: menu (connect QR)");
    return;
  }
  if (uiScreenLockActive()) {
    uiScreenPetShow();
    Serial.println("BACK: pet (code pad cancel)");
    return;
  }
  if (uiScreenOtaActive()) {
    uiScreenOtaCancel();   // abort the update, back to the menu
    Serial.println("BACK: ota abort");
    return;
  }
  if (uiScreenDcaDetailOpen()) {
    uiScreenDcaCloseDetail();
    Serial.println("BACK: close detail");
    return;
  }
  lv_obj_t* act = lv_screen_active();
  if (act == g_ui.dcaScreen) {
    uiScreenPetShow();
    Serial.println("BACK: pet screen");
  } else if (act == g_ui.menuScreen) {
    uiScreenMenuBack();
    Serial.println("BACK: menu back");
  } else if (act == g_ui.createScreen) {
    uiScreenCreateBack();
    Serial.println("BACK: create back");
  }
}

#ifdef FINAGOTCHI_PCB_V1
// Crown CCW = navigate backward: previous card/row/option; rotating past
// the top of a list backs out of the screen (menu -> pet, portfolio ->
// pet, create summary -> previous drum).
void onNavPrev() {
  if (uiScreenLockActive()) { uiScreenLockPrev(); return; }
  // connect QR prompt: either direction toggles the iOS/Android tab
  if (uiScreenConnectActive()) { uiScreenConnectToggle(); return; }
  if (uiScreenDcaDetailOpen()) {
    uiScreenDcaCloseDetail();
    Serial.println("NAV back: close detail");
    return;
  }
  lv_obj_t* act = lv_screen_active();
  if (act == g_ui.petScreen) {
    uiScreenMenuShow();   // the cycle backwards: pet <- menu
    Serial.println("NAV back: menu screen");
  } else if (act == g_ui.dcaScreen) {
    if (!uiScreenDcaFocusPrev()) {
      uiScreenPetShow();
      Serial.println("NAV back: pet screen");
    } else {
      Serial.println("NAV back: focus previous card");
    }
  } else if (act == g_ui.menuScreen) {
    if (!uiScreenMenuFocusPrev()) {
      uiScreenMenuBack();
      Serial.println("NAV back: menu back");
    } else {
      Serial.println("NAV back: focus previous row");
    }
  } else if (act == g_ui.createScreen) {
    uiScreenCreateFocusPrev();
    Serial.println("NAV back: create: previous option");
  }
}

// Quadrature decode, polled from uiInputUpdate (loop rate is kHz-class —
// plenty for a thumb wheel). One detent = a full 4-transition quadrature
// cycle; if the dial feels reversed on the assembled board, flip
// CROWN_INVERT (or swap the A/B wires).
uint8_t crownState = 0;   // (A<<1)|B
int8_t  crownAcc   = 0;
constexpr int8_t CROWN_TRANSITIONS_PER_STEP = 4;
constexpr bool   CROWN_INVERT = false;
// index = (old<<2)|new; 0 = no move, ±1 = single transition
const int8_t kQem[16] = { 0,-1,1,0, 1,0,0,-1, -1,0,0,1, 0,1,-1,0 };

void crownStep(int8_t dir) {
  if (ui::afkInput()) return;   // first detent after a nap only wakes
  if (CROWN_INVERT) dir = -dir;
  feedback::click();          // detent tick: beep + motor tap
  if (dir > 0) onNavShort();
  else onNavPrev();
}

void pollCrown() {
  uint8_t s = (digitalRead(CROWN_A_PIN) << 1) | digitalRead(CROWN_B_PIN);
  if (s == crownState) return;
  crownAcc += kQem[(crownState << 2) | s];
  crownState = s;
  if (crownAcc >= CROWN_TRANSITIONS_PER_STEP) {
    crownAcc = 0;
    crownStep(+1);
  } else if (crownAcc <= -CROWN_TRANSITIONS_PER_STEP) {
    crownAcc = 0;
    crownStep(-1);
  }
}
#endif // FINAGOTCHI_PCB_V1

} // namespace

namespace ui {

// Boot recovery: both breadboard buttons, or a deliberate crown-only hold
// on PCB v1 so a damaged/disconnected cancel switch cannot lock recovery out.
bool recoveryRequested() {
#ifdef FINAGOTCHI_PCB_V1
  pinMode(CROWN_PUSH_PIN, INPUT_PULLUP);
  delay(5);
  if (digitalRead(CROWN_PUSH_PIN) != LOW) return false;
  uint32_t start = millis();
  while (millis() - start < 4000) {
    if (digitalRead(CROWN_PUSH_PIN) != LOW) return false;
    delay(20);
  }
  return true;
#else
  pinMode(BUTTON_1_PIN, INPUT_PULLUP);
  pinMode(BUTTON_2_PIN, INPUT_PULLUP);
  delay(5);   // let the pull-ups settle
  return digitalRead(BUTTON_1_PIN) == LOW && digitalRead(BUTTON_2_PIN) == LOW;
#endif
}

} // namespace ui

void uiInputInit() {
#ifdef FINAGOTCHI_PCB_V1
  pinMode(CROWN_A_PIN, INPUT_PULLUP);
  pinMode(CROWN_B_PIN, INPUT_PULLUP);
  pinMode(btnPush.pin, INPUT_PULLUP);
  pinMode(btnCancel.pin, INPUT_PULLDOWN);
  crownState = (digitalRead(CROWN_A_PIN) << 1) | digitalRead(CROWN_B_PIN);
#else
  pinMode(btn1.pin, INPUT_PULLUP);
  pinMode(btn2.pin, INPUT_PULLUP);
#endif

  lv_indev_t* indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_KEYPAD);
  lv_indev_set_read_cb(indev, keypadRead);
  lv_indev_set_group(indev, g_ui.group);

#ifdef FINAGOTCHI_PCB_V1
  Serial.printf("Input: crown A/B/push GPIO%d/%d/%d + optional cancel GPIO%d "
                "(rotate = navigate, short = action, long = back)\n",
                CROWN_A_PIN, CROWN_B_PIN, CROWN_PUSH_PIN, CANCEL_PIN);
#else
  Serial.printf("Buttons: GPIO%d (left: next, long: confirm) + GPIO%d "
                "(right: back/menu, long: confirm)\n", btn1.pin, btn2.pin);
#endif
}

void uiInputUpdate() {
  float nowSec = millis() / 1000.0f;

#ifdef FINAGOTCHI_PCB_V1
  pollCrown();   // CW detent = navigate forward, CCW = backward

  bool pushLong, cancelLong;
  bool pushShort   = handleButton(btnPush, pushLong);
  bool cancelShort = handleButton(btnCancel, cancelLong);

  if (cancelShort && !ui::afkInput()) { feedback::back(); onBack(); }
  if (pushShort && !ui::afkInput()) {
    feedback::confirm();
    onConfirm(nowSec);
  }
  if (pushLong && !ui::afkInput()) {
    feedback::back();
    onBack();
  }
  (void)cancelLong;
#else
  bool long1, long2;
  bool short1 = handleButton(btn1, long1);
  bool short2 = handleButton(btn2, long2);

  if (short1 && !ui::afkInput()) onNavShort();
  if (short2 && !ui::afkInput()) {
    if (lv_screen_active() == g_ui.petScreen) uiScreenMenuShow();
    else onBack();
  }
  if ((long1 || long2) && !ui::afkInput()) onConfirm(nowSec);
#endif
}
