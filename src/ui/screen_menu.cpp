/*
  screen_menu.cpp — LVGL menu screen (third screen in the pet -> portfolio
  -> menu cycle): a vertical, scrollable APP LIST, launcher style.

    [icon] Feed pet          |
    [icon] Accessory         |  <- slim cyan scrollbar, right edge
    [icon] Mood              |
    [icon] Open DCA          |

  - each row: circular icon chip (border-token ring) + app name label;
    rows scroll vertically, clipping at the top/bottom edges; the focused
    row gets the cyan ring + hover-surface tint and is auto-scrolled into
    view (lv_obj_scroll_to_view)
  - reserved right zone (~25%): when the list is fully at the top (i.e. on
    entry — navigation is forward-only), a BACK affordance lives there: a
    right-pointing arrow chip, focused first; activating it returns to the
    pet screen. Once the user walks into the list, the zone instead shows a
    short muted info word for the focused app ("happy +5", the current
    accessory/mood name, "SOL->USDC")
  - BTN short (either button) walks the focus down the rows; past the last
    row uiScreenMenuFocusAdvance returns false and input.cpp moves on to
    the pet screen (contract unchanged). BTN long (either button) runs the
    focused app with the ~150 ms press pulse; the back arrow exits to the
    pet screen

  The focus group only ever holds the visible objects (uiGroupSet
  discipline): the arrow while shown, then exactly the 4 rows. The
  BLE/state side of every action lives in main.cpp (ui::Actions); this
  file owns only the widgets and the walking.
*/

#include "ui_internal.h"

namespace {

constexpr uint8_t ROW_ACCESSORY = 0;
constexpr uint8_t ROW_MOOD = 1;
constexpr uint8_t ROW_FEED = 2;
constexpr uint8_t ROW_OPEN_DCA = 3;
constexpr uint8_t ROW_COUNT = 4;

// Current accessory/mood names, mirrored from main.cpp (ui::setMenu*) —
// shown as the focused app's info word in the right zone.
char accessoryName[12] = "none";
char moodName[12] = "calm";

lv_obj_t* menuScr;
lv_obj_t* list;
lv_obj_t* row[ROW_COUNT];
lv_obj_t* backArrow;    // right-zone back affordance (visible at top)
lv_obj_t* infoLabel;    // right-zone per-app info (visible otherwise)

const lv_font_t* CHIP_FONT = &lv_font_montserrat_16;

lv_obj_t* focusedRow() {
  return lv_group_get_focused(g_ui.group);
}

// Right-zone info for the focused app: a short state word, not a hint.
void infoFill() {
  lv_obj_t* f = focusedRow();
  const char* text = "";
  if (f == row[ROW_FEED])          text = "happy +5";
  else if (f == row[ROW_ACCESSORY]) text = accessoryName;
  else if (f == row[ROW_MOOD])      text = moodName;
  else if (f == row[ROW_OPEN_DCA])  text = "SOL->USDC";
  lv_label_set_text(infoLabel, text);
  lv_obj_set_hidden(infoLabel, false);
}

void rowFocused(lv_event_t*) {
  lv_obj_t* f = focusedRow();
  if (f == backArrow) return;   // zone shows the arrow itself
  lv_obj_scroll_to_view(f, LV_ANIM_ON);
  infoFill();
}

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
  lv_obj_t* t = lv_event_get_target_obj(e);
  if (t == backArrow) { uiScreenPetShow(); return; }
  uint8_t idx = static_cast<uint8_t>(
      reinterpret_cast<intptr_t>(lv_obj_get_user_data(t)));
  pressPulse(row[idx]);
  activate(idx, millis() / 1000.0f);
}

} // namespace

lv_obj_t* uiScreenMenuCreate() {
  menuScr = lv_obj_create(nullptr);
  uiThemeScreen(menuScr);

  // --- scrollable app list (left ~70%) ---
  list = lv_obj_create(menuScr);
  lv_obj_set_size(list, 160, 218);
  lv_obj_align(list, LV_ALIGN_TOP_LEFT, 16, 14);
  lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(list, 0, 0);
  lv_obj_set_style_pad_all(list, 2, 0);
  lv_obj_set_style_pad_row(list, 8, 0);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);
  // The "green thing" from the mock, in theme primary: slim cyan track on
  // the right edge that travels/shrinks with the content.
  lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_set_style_bg_color(list, UI_COL_PRIMARY, LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_SCROLLBAR);
  lv_obj_set_style_width(list, 3, LV_PART_SCROLLBAR);
  lv_obj_set_style_radius(list, 2, LV_PART_SCROLLBAR);
  lv_obj_set_style_pad_right(list, 6, 0);

  static const char* NAMES[ROW_COUNT] = {
    "Accessory", "Mood", "Feed pet", "Open DCA"
  };
  static const char* GLYPHS[ROW_COUNT] = {
    LV_SYMBOL_IMAGE,   // Accessory
    ":)",              // Mood (smiley monogram)
    LV_SYMBOL_PLUS,    // Feed (primary)
    LV_SYMBOL_BARS,    // Open DCA (chart)
  };
  for (uint8_t i = 0; i < ROW_COUNT; i++) {
    bool primary = (i == ROW_FEED);
    row[i] = lv_obj_create(list);
    lv_obj_set_size(row[i], 146, 56);
    lv_obj_set_style_bg_color(row[i], UI_COL_SURFACE, 0);
    lv_obj_set_style_bg_color(row[i], UI_COL_SURFACE_HI, LV_STATE_FOCUSED);
    lv_obj_set_style_bg_opa(row[i], LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row[i], UI_RADIUS_CARD, 0);
    lv_obj_set_style_border_width(row[i], 1, 0);
    lv_obj_set_style_border_color(row[i], UI_COL_BORDER, 0);
    lv_obj_set_style_border_width(row[i], 2, LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(row[i], UI_COL_PRIMARY, LV_STATE_FOCUSED);
    lv_obj_set_style_pad_all(row[i], 0, 0);
    lv_obj_set_scrollable(row[i], false);
    lv_obj_set_clickable(row[i], true);
    lv_obj_set_user_data(row[i], reinterpret_cast<void*>(static_cast<intptr_t>(i)));
    lv_obj_add_event_cb(row[i], rowClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(row[i], rowFocused, LV_EVENT_FOCUSED, nullptr);

    // Circular icon chip on the left; Feed keeps the primary treatment.
    lv_obj_t* chip = lv_obj_create(row[i]);
    lv_obj_set_size(chip, 38, 38);
    lv_obj_align(chip, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(chip, primary ? UI_COL_PRIMARY : UI_COL_SURFACE_HI, 0);
    lv_obj_set_style_border_width(chip, 1, 0);
    lv_obj_set_style_border_color(chip, UI_COL_BORDER, 0);
    lv_obj_set_style_border_color(chip, UI_COL_PRIMARY, LV_STATE_FOCUSED);
    lv_obj_set_style_pad_all(chip, 0, 0);
    lv_obj_set_scrollable(chip, false);
    lv_obj_set_clickable(chip, false);

    lv_obj_t* g = uiThemeLabel(chip, CHIP_FONT, primary ? UI_COL_BG : UI_COL_TEXT);
    lv_label_set_text(g, GLYPHS[i]);
    lv_obj_center(g);

    lv_obj_t* name = uiThemeLabel(row[i], &lv_font_montserrat_14, UI_COL_TEXT);
    lv_label_set_text(name, NAMES[i]);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 54, 0);
  }

  // --- reserved right zone: back affordance + per-app info ---
  backArrow = lv_obj_create(menuScr);
  lv_obj_set_size(backArrow, 40, 40);
  lv_obj_align(backArrow, LV_ALIGN_TOP_RIGHT, -14, 52);
  lv_obj_set_style_radius(backArrow, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(backArrow, UI_COL_SURFACE, 0);
  lv_obj_set_style_bg_opa(backArrow, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(backArrow, 1, 0);
  lv_obj_set_style_border_color(backArrow, UI_COL_PRIMARY, 0);
  lv_obj_set_style_border_width(backArrow, 3, LV_STATE_FOCUSED);
  lv_obj_set_style_pad_all(backArrow, 0, 0);
  lv_obj_set_scrollable(backArrow, false);
  lv_obj_set_clickable(backArrow, true);
  lv_obj_add_event_cb(backArrow, rowClicked, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* arrowGlyph = uiThemeLabel(backArrow, &lv_font_montserrat_20, UI_COL_PRIMARY);
  lv_label_set_text(arrowGlyph, LV_SYMBOL_RIGHT);
  lv_obj_center(arrowGlyph);

  infoLabel = uiThemeLabel(menuScr, &lv_font_montserrat_12, UI_COL_MUTED);
  lv_obj_align(infoLabel, LV_ALIGN_TOP_RIGHT, -14, 104);
  lv_obj_set_hidden(infoLabel, true);

  return menuScr;
}

void uiScreenMenuShow() {
  // Enter at the top: back affordance focused (long-press/action = pet
  // screen); the rows follow in the focus chain. Scrolling down swaps the
  // arrow out of the group (see uiScreenMenuFocusAdvance).
  lv_obj_t* visible[ROW_COUNT + 1];
  visible[0] = backArrow;
  for (uint8_t i = 0; i < ROW_COUNT; i++) visible[i + 1] = row[i];
  lv_obj_set_hidden(backArrow, false);
  lv_obj_set_hidden(infoLabel, true);
  lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
  uiGroupSet(visible, ROW_COUNT + 1, false);   // focuses the arrow
  lv_screen_load_anim(menuScr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
}

void uiScreenMenuActivate(float nowSec) {
  lv_obj_t* f = focusedRow();
  if (!f) return;
  if (f == backArrow) {
    uiScreenPetShow();
    return;
  }
  uint8_t idx = static_cast<uint8_t>(
      reinterpret_cast<intptr_t>(lv_obj_get_user_data(f)));
  pressPulse(row[idx]);
  activate(idx, nowSec);
}

bool uiScreenMenuFocusAdvance() {
  lv_obj_t* f = focusedRow();
  if (!f) {
    lv_group_focus_obj(row[0]);
    infoFill();
    return true;
  }
  if (f == backArrow) {
    // Walk into the list: the back affordance leaves the focus group and
    // the right zone switches to per-app info until the next entry.
    lv_obj_set_hidden(backArrow, true);
    uiGroupSet(row, ROW_COUNT, false);   // focuses row[0]
    lv_obj_scroll_to_view(row[0], LV_ANIM_ON);
    infoFill();
    return true;
  }
  if (f == row[ROW_COUNT - 1]) return false;   // past the last row: pet screen
  lv_group_focus_next(g_ui.group);
  return true;   // rowFocused scrolls it into view + updates the info
}

void uiScreenMenuSetAccessory(const char* name) {
  strlcpy(accessoryName, name, sizeof(accessoryName));
  if (!lv_obj_is_hidden(infoLabel) &&
      focusedRow() == row[ROW_ACCESSORY]) infoFill();
}

void uiScreenMenuSetMood(const char* name) {
  strlcpy(moodName, name, sizeof(moodName));
  if (!lv_obj_is_hidden(infoLabel) &&
      focusedRow() == row[ROW_MOOD]) infoFill();
}
