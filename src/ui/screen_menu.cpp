/*
  screen_menu.cpp — LVGL menu screen (third screen in the pet -> portfolio
  -> menu cycle), mirroring the mobile app's bottom action bar ("PetMenu",
  app/(tabs)/index.tsx): one horizontal row of small round icon buttons with
  ONE primary action (Feed) as a filled cyan circle with a dark glyph.

    ( Accessory ) ( Mood ) (  FEED +  ) ( Open DCA )

  - above the row a single muted label names the FOCUSED action and its
    state ("Accessory: Crown", "Mood: Happy", "Feed pet", "Open DCA") —
    icons alone are ambiguous
  - surface circles with a border token ring; the focused button gets a
    thick cyan ring (white on the cyan Feed button so it stays visible)
  - BTN2 short walks the focus left -> right through the 4 and moves on to
    the pet screen after the last; BTN1 short runs the focused action with
    a ~150 ms scale pulse (the app's PressableScale); either button
    long-press escapes to the pet screen

  All 4 buttons are always visible, so the focus group holds exactly these
  4 (uiGroupSet discipline). The BLE/state side of every action lives in
  main.cpp (ui::Actions); this file owns only the widgets and the walking.
*/

#include "ui_internal.h"

namespace {

constexpr uint8_t ROW_ACCESSORY = 0;
constexpr uint8_t ROW_MOOD = 1;
constexpr uint8_t ROW_FEED = 2;
constexpr uint8_t ROW_OPEN_DCA = 3;
constexpr uint8_t ROW_COUNT = 4;

// Current accessory/mood names, mirrored from main.cpp (ui::setMenu*).
char accessoryName[12] = "none";
char moodName[12] = "calm";

// Geometry: 42 px icon circles, 54 px primary circle, 8 px gaps -> 204 px
// row centered on the panel's vertical midline (the round panel's widest
// line, so the row sits fully inside the inscribed circle).
constexpr int32_t ICON_SIZE = 42;
constexpr int32_t FEED_SIZE = 54;
constexpr int32_t ROW_Y = 120;   // vertical center of the row
const int32_t ROW_X[ROW_COUNT] = { 39, 89, 145, 201 };   // button centers

lv_obj_t* menuScr;
lv_obj_t* row[ROW_COUNT];
lv_obj_t* focusLabel;

// Focused-action caption above the row. Accessory/mood carry their state.
void focusLabelFill() {
  lv_obj_t* f = lv_group_get_focused(g_ui.group);
  if (!f) return;
  uint8_t idx = static_cast<uint8_t>(
      reinterpret_cast<intptr_t>(lv_obj_get_user_data(f)));
  char buf[28];
  switch (idx) {
    case ROW_ACCESSORY:
      snprintf(buf, sizeof(buf), "Accessory: %s", accessoryName);
      buf[11] = static_cast<char>(toupper(buf[11]));
      break;
    case ROW_MOOD:
      snprintf(buf, sizeof(buf), "Mood: %s", moodName);
      buf[6] = static_cast<char>(toupper(buf[6]));
      break;
    case ROW_FEED:     strlcpy(buf, "Feed pet", sizeof(buf)); break;
    default:           strlcpy(buf, "Open DCA", sizeof(buf)); break;
  }
  lv_label_set_text(focusLabel, buf);
}

void rowFocused(lv_event_t*) { focusLabelFill(); }

// ~150 ms scale pulse on activation (app's PressableScale).
void pressPulse(lv_obj_t* obj) {
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, obj);
  lv_anim_set_values(&a, 256, 216);
  lv_anim_set_duration(&a, 75);
  lv_anim_set_playback_duration(&a, 75);
  lv_anim_set_exec_cb(&a, [](void* o, int32_t v) {
    lv_obj_set_style_transform_scale(static_cast<lv_obj_t*>(o), v, 0);
  });
  lv_anim_start(&a);
}

void activate(uint8_t idx, float nowSec) {
  ui::Actions& a = g_ui.actions;
  switch (idx) {
    case ROW_FEED:      if (a.feedPet) a.feedPet(nowSec); break;
    case ROW_ACCESSORY: if (a.cycleItem) a.cycleItem(); break;
    case ROW_MOOD:      if (a.cycleMood) a.cycleMood(nowSec); break;
    case ROW_OPEN_DCA:  if (a.openDca) a.openDca(); break;
  }
}

void rowClicked(lv_event_t* e) {
  // Keypad ENTER delivers LV_EVENT_CLICKED on the focused row.
  uint8_t idx = static_cast<uint8_t>(
      reinterpret_cast<intptr_t>(lv_obj_get_user_data(lv_event_get_target_obj(e))));
  pressPulse(row[idx]);
  activate(idx, millis() / 1000.0f);
}

} // namespace

lv_obj_t* uiScreenMenuCreate() {
  menuScr = lv_obj_create(nullptr);
  uiThemeScreen(menuScr);

  // Focused-action caption (updated by rowFocused on every focus move).
  focusLabel = uiThemeLabel(menuScr, &lv_font_montserrat_14, UI_COL_MUTED);
  lv_obj_align(focusLabel, LV_ALIGN_TOP_MID, 0, 76);

  static const char* GLYPHS[ROW_COUNT] = {
    LV_SYMBOL_IMAGE,   // Accessory
    ":)",              // Mood (smiley monogram)
    LV_SYMBOL_PLUS,    // Feed (primary)
    LV_SYMBOL_BARS,    // Open DCA (chart)
  };
  for (uint8_t i = 0; i < ROW_COUNT; i++) {
    bool primary = (i == ROW_FEED);
    int32_t size = primary ? FEED_SIZE : ICON_SIZE;
    row[i] = lv_obj_create(menuScr);
    lv_obj_set_size(row[i], size, size);
    lv_obj_align(row[i], LV_ALIGN_TOP_LEFT,
                 ROW_X[i] - size / 2, ROW_Y - size / 2);
    lv_obj_set_style_radius(row[i], LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(row[i], LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(row[i], primary ? UI_COL_PRIMARY : UI_COL_SURFACE, 0);
    lv_obj_set_style_bg_color(row[i], primary ? UI_COL_PRIMARY : UI_COL_SURFACE_HI,
                              LV_STATE_PRESSED);
    lv_obj_set_style_border_width(row[i], 1, 0);
    lv_obj_set_style_border_color(row[i], UI_COL_BORDER, 0);
    // Main nav surface: the focus ring is thicker than on list cards —
    // white on the cyan primary button so it stays visible.
    lv_obj_set_style_border_width(row[i], 3, LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(row[i], primary ? UI_COL_TEXT : UI_COL_PRIMARY,
                                  LV_STATE_FOCUSED);
    lv_obj_set_style_pad_all(row[i], 0, 0);
    lv_obj_set_scrollable(row[i], false);
    lv_obj_set_clickable(row[i], true);
    lv_obj_set_user_data(row[i], reinterpret_cast<void*>(static_cast<intptr_t>(i)));
    lv_obj_add_event_cb(row[i], rowClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(row[i], rowFocused, LV_EVENT_FOCUSED, nullptr);

    lv_obj_t* g = uiThemeLabel(row[i], primary ? &lv_font_montserrat_28
                                               : &lv_font_montserrat_16,
                               primary ? UI_COL_BG : UI_COL_TEXT);
    lv_label_set_text(g, GLYPHS[i]);
    lv_obj_center(g);
  }

  return menuScr;
}

void uiScreenMenuShow() {
  uiGroupSet(row, ROW_COUNT, false);
  focusLabelFill();
  lv_screen_load_anim(menuScr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
}

void uiScreenMenuActivate(float nowSec) {
  lv_obj_t* f = lv_group_get_focused(g_ui.group);
  if (!f) return;
  uint8_t idx = static_cast<uint8_t>(
      reinterpret_cast<intptr_t>(lv_obj_get_user_data(f)));
  pressPulse(row[idx]);
  activate(idx, nowSec);
}

bool uiScreenMenuFocusAdvance() {
  lv_obj_t* f = lv_group_get_focused(g_ui.group);
  if (f && f != row[ROW_COUNT - 1]) {
    lv_group_focus_next(g_ui.group);
    return true;
  }
  return false;   // past the last button: caller moves to the pet screen
}

void uiScreenMenuSetAccessory(const char* name) {
  strlcpy(accessoryName, name, sizeof(accessoryName));
  lv_obj_t* f = lv_group_get_focused(g_ui.group);
  if (f == row[ROW_ACCESSORY]) focusLabelFill();
}

void uiScreenMenuSetMood(const char* name) {
  strlcpy(moodName, name, sizeof(moodName));
  lv_obj_t* f = lv_group_get_focused(g_ui.group);
  if (f == row[ROW_MOOD]) focusLabelFill();
}
