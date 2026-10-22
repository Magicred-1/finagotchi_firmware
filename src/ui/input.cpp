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

  BACK/CANCEL (right button double-press, universal): cancels the code
  pad, closes the detail view, leaves the current screen — the menu's
  cosmetics page steps back to the apps page first, the create wheel
  steps back one drum. No-op on the pet screen (already home).

  PCB v1 (FINAGOTCHI_PCB_V1) replaces the two buttons with a crown
  encoder + a dedicated cancel button:

    crown rotate CW  = navigate forward (same as short press)
    crown rotate CCW = navigate backward: previous card/row/option,
                       past the top of a list backs out of the screen
    crown push       = confirm / trigger the focused action (same as
                       long press); double-push keeps the bonus extras
                       (pet reaction / USD <-> SOL / code-pad cancel)
    cancel (GPIO7)   = BACK, single press, no double needed

  The focus group only ever holds the visible objects of the ACTIVE screen
  (see uiGroupSet) — hidden cards never swallow key presses. Card/row
  activation goes through LV_KEY_ENTER on the keypad indev so focus visuals
  come from the framework.
*/

#include "ui_internal.h"

namespace {

#ifdef FINAGOTCHI_PCB_V1
// PCB v1: crown encoder (quadrature A/B + active-LOW push, 10k pull-ups on
// the board) and a dedicated active-HIGH cancel button (10k pull-down).
constexpr uint8_t CROWN_A_PIN    = 15;
constexpr uint8_t CROWN_B_PIN    = 16;
constexpr uint8_t CROWN_PUSH_PIN = 4;    // same pin/polarity as breadboard BTN1
constexpr uint8_t CANCEL_PIN     = 7;
#else
// Hardware reality: captures have shown each button electrically dead at
// different times (flaky unit), so BOTH buttons carry the same mapping —
// short = navigate, long = action — and the UI works with either one.
constexpr uint8_t BUTTON_1_PIN = 4;    // left button
constexpr uint8_t BUTTON_2_PIN = 37;   // right button
#endif

constexpr uint32_t DEBOUNCE_MS    = 30;
constexpr uint32_t LONG_PRESS_MS  = 1000;
constexpr uint32_t DOUBLE_PRESS_MS = 500;

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
  if (uiScreenLockActive()) { uiScreenLockNext(); return; }
  lv_obj_t* act = lv_screen_active();
  if (uiScreenDcaDetailOpen()) {
    onDetailShort();
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

// Long press (either button) = the action of the current screen.
void onActionLong(float nowSec) {
  if (uiScreenLockActive()) { uiScreenLockConfirm(); return; }
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

// BTN2 double-press = BACK, universal: cancel the code pad, close the
// detail, leave the current screen (menu cosmetics page steps back to the
// apps page, create wheel steps back a drum). Pet screen = home, so back
// is a no-op there.
void onBack() {
  if (uiScreenLockActive()) {
    uiScreenPetShow();
    Serial.println("BACK: pet (code pad cancel)");
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
// the top of a list backs out of the screen (menu: apps page -> pet,
// cosmetics -> apps; portfolio -> pet; create summary -> previous drum).
void onNavPrev() {
  if (uiScreenLockActive()) { uiScreenLockPrev(); return; }
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
  if (CROWN_INVERT) dir = -dir;
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

// Bonus: left-button double-press extras (when the left button is alive).
void onBtn1Double(float nowSec) {
  lv_obj_t* act = lv_screen_active();
  if (uiScreenLockActive()) {   // code pad escape hatch: double = cancel
    uiScreenPetShow();
    Serial.println("lock: cancel");
    return;
  }
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
  if (uiScreenLockActive()) return true;                        // cancel
  lv_obj_t* act = lv_screen_active();
  if (act == g_ui.petScreen) return true;                       // reaction
  if (act == g_ui.dcaScreen && !uiScreenDcaDetailOpen()) return true;  // USD/SOL
  return false;
}

} // namespace

namespace ui {

// Boot-time recovery chord query (called from setup() before ui::begin):
// true while BOTH inputs are held. Used to erase stale BLE bonds and the
// parental code. Breadboard: the two buttons. PCB v1: crown push + cancel.
bool bothButtonsHeld() {
#ifdef FINAGOTCHI_PCB_V1
  pinMode(CROWN_PUSH_PIN, INPUT_PULLUP);
  pinMode(CANCEL_PIN, INPUT_PULLDOWN);
  delay(5);   // let the pulls settle
  return digitalRead(CROWN_PUSH_PIN) == LOW && digitalRead(CANCEL_PIN) == HIGH;
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
  Serial.printf("Input: crown A/B/push GPIO%d/%d/%d + cancel GPIO%d "
                "(rotate = navigate, push = action, cancel = back)\n",
                CROWN_A_PIN, CROWN_B_PIN, CROWN_PUSH_PIN, CANCEL_PIN);
#else
  Serial.printf("Buttons: GPIO%d + GPIO%d (short = navigate, long = action)\n",
                btn1.pin, btn2.pin);
#endif
}

void uiInputUpdate() {
  float nowSec = millis() / 1000.0f;

  // Deferred detail close: the double window expired without a second
  // press, so it was a plain short — close the detail (if still open).
  if (detailShortAt && millis() - detailShortAt >= DOUBLE_PRESS_MS) {
    detailShortAt = 0;
    if (uiScreenDcaDetailOpen()) {
      uiScreenDcaCloseDetail();
      Serial.println("detail: close");
    }
  }

#ifdef FINAGOTCHI_PCB_V1
  pollCrown();   // CW detent = navigate forward, CCW = backward

  bool pushLong, cancelLong;
  bool pushShort   = handleButton(btnPush, pushLong);
  bool cancelShort = handleButton(btnCancel, cancelLong);

  if (cancelShort) onBack();   // dedicated back/cancel: single press

  // Crown push = confirm / trigger the focused action (the long-press
  // semantics on the breadboard). Double-push keeps the bonus extras
  // (pet reaction / USD <-> SOL / code-pad cancel) where they exist.
  static uint32_t lastPushShort = 0;
  if (pushShort) {
    if (btn1HasDoubleHere() && millis() - lastPushShort < DOUBLE_PRESS_MS) {
      lastPushShort = 0;
      onBtn1Double(nowSec);
    } else {
      lastPushShort = millis();
      onActionLong(nowSec);
    }
  }

  if (pushLong) onActionLong(nowSec);
  (void)cancelLong;
#else
  bool long1, long2;
  bool short1 = handleButton(btn1, long1);
  bool short2 = handleButton(btn2, long2);

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

  // BTN2 double = BACK (universal): cancel the code pad / close detail /
  // leave the screen. Singles keep navigating — fully redundant with BTN1.
  static uint32_t lastShort2 = 0;
  if (short2) {
    if (millis() - lastShort2 < DOUBLE_PRESS_MS) {
      lastShort2 = 0;
      onBack();
    } else {
      lastShort2 = millis();
      onNavShort();
    }
  }

  if (long1) onActionLong(nowSec);

  if (long2) onActionLong(nowSec);
#endif
}
