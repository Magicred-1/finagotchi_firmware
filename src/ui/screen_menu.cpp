/*
  screen_menu.cpp — LVGL menu screen (third screen in the pet -> portfolio
  -> menu cycle): four focusable action rows in the same card style as the
  portfolio (cyan focus ring). BTN2 short walks the focus down the rows and
  moves on to the pet screen after the last one; BTN1 short runs the
  focused row; either button long-press escapes to the pet screen.

  Rows:
    Feed pet        — BLE feed:req to the app (toast when offline)
    Accessory: <n>  — cycle the pet's collectible (local + item: push)
    Mood: <n>       — cycle the pet's mood (local + mood: push)
    Open DCA        — BLE dca:req to the app (toast when offline)

  The BLE/state side of every row lives in main.cpp (ui::Actions); this
  file owns only the widgets and the focus walking.
*/

#include "ui_internal.h"

namespace {

constexpr uint8_t ROW_FEED = 0;
constexpr uint8_t ROW_ACCESSORY = 1;
constexpr uint8_t ROW_MOOD = 2;
constexpr uint8_t ROW_OPEN_DCA = 3;
constexpr uint8_t ROW_COUNT = 4;

lv_obj_t* menuScr;
lv_obj_t* row[ROW_COUNT];
lv_obj_t* rowValue[ROW_COUNT];   // right-side value labels (rows 1/2 only)

void rowClicked(lv_event_t* e) {
  // Touch/pad activation path parity with the keypad ENTER (BTN1).
  uint8_t idx = static_cast<uint8_t>(
      reinterpret_cast<intptr_t>(lv_obj_get_user_data(lv_event_get_target_obj(e))));
  ui::Actions& a = g_ui.actions;
  float nowSec = millis() / 1000.0f;
  switch (idx) {
    case ROW_FEED:      if (a.feedPet) a.feedPet(nowSec); break;
    case ROW_ACCESSORY: if (a.cycleItem) a.cycleItem(); break;
    case ROW_MOOD:      if (a.cycleMood) a.cycleMood(nowSec); break;
    case ROW_OPEN_DCA:  if (a.openDca) a.openDca(); break;
  }
}

} // namespace

lv_obj_t* uiScreenMenuCreate() {
  menuScr = lv_obj_create(nullptr);
  uiThemeScreen(menuScr);

  lv_obj_t* title = uiThemeLabel(menuScr, &lv_font_montserrat_12, UI_COL_MUTED);
  lv_label_set_text(title, "MENU");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 14);

  static const char* ROW_TITLES[ROW_COUNT] = {
    "Feed pet", "Accessory", "Mood", "Open DCA"
  };
  for (uint8_t i = 0; i < ROW_COUNT; i++) {
    row[i] = lv_obj_create(menuScr);
    lv_obj_set_size(row[i], 190, 36);
    lv_obj_align(row[i], LV_ALIGN_TOP_MID, 0, 34 + i * 42);
    uiThemeCard(row[i]);
    lv_obj_set_style_radius(row[i], UI_RADIUS_SMALL, 0);
    lv_obj_set_clickable(row[i], true);
    lv_obj_set_user_data(row[i], reinterpret_cast<void*>(static_cast<intptr_t>(i)));
    lv_obj_add_event_cb(row[i], rowClicked, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* name = uiThemeLabel(row[i], &lv_font_montserrat_14, UI_COL_TEXT);
    lv_label_set_text(name, ROW_TITLES[i]);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, UI_SP3, 0);

    if (i == ROW_ACCESSORY || i == ROW_MOOD) {
      rowValue[i] = uiThemeLabel(row[i], &lv_font_montserrat_12, UI_COL_MUTED);
      lv_label_set_text(rowValue[i], "-");
      lv_obj_align(rowValue[i], LV_ALIGN_RIGHT_MID, -UI_SP3, 0);
    } else {
      rowValue[i] = nullptr;
    }
  }

  uiHintBarSet(uiHintBarCreate(menuScr, -6), "1: select   2: next\nhold: pet");
  return menuScr;
}

void uiScreenMenuShow() {
  uiGroupSet(row, ROW_COUNT, false);
  lv_screen_load_anim(menuScr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
}

void uiScreenMenuActivate(float nowSec) {
  lv_obj_t* f = lv_group_get_focused(g_ui.group);
  if (!f) return;
  uint8_t idx = static_cast<uint8_t>(
      reinterpret_cast<intptr_t>(lv_obj_get_user_data(f)));
  ui::Actions& a = g_ui.actions;
  switch (idx) {
    case ROW_FEED:      if (a.feedPet) a.feedPet(nowSec); break;
    case ROW_ACCESSORY: if (a.cycleItem) a.cycleItem(); break;
    case ROW_MOOD:      if (a.cycleMood) a.cycleMood(nowSec); break;
    case ROW_OPEN_DCA:  if (a.openDca) a.openDca(); break;
  }
}

bool uiScreenMenuFocusAdvance() {
  lv_obj_t* f = lv_group_get_focused(g_ui.group);
  if (f && f != row[ROW_COUNT - 1]) {
    lv_group_focus_next(g_ui.group);
    return true;
  }
  return false;   // past the last row: caller moves to the next screen
}

void uiScreenMenuSetAccessory(const char* name) {
  lv_label_set_text(rowValue[ROW_ACCESSORY], name);
}

void uiScreenMenuSetMood(const char* name) {
  lv_label_set_text(rowValue[ROW_MOOD], name);
}
