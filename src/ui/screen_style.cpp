/*
  screen_style.cpp — clothes and mood picker dialogs.

  Both pickers use the same watch-style list: crown rotates, push selects,
  long-push/cancel returns to Apps. The active choice starts focused and the
  saved choice keeps a checkmark. Selection updates the normal BLE snapshot,
  plays a two-tone confirmation, then returns to the pet so the result is
  immediately visible. Widgets are built lazily and freed on exit.
*/

#include "ui_internal.h"
#include "feedback.h"

namespace {

struct StyleOption {
  const char* name;
  const char* glyph;
  uint32_t accent;
};

const StyleOption CLOTHES[] = {
  { "None",    LV_SYMBOL_CLOSE,    0x98989F },
  { "Crown",   "C",                0xFFD60A },
  { "Glasses", LV_SYMBOL_EYE_OPEN, 0x64D2FF },
  { "Bow tie", "B",                0xFF453A },
  { "Halo",    "O",                0xFFD60A },
  { "Diamond", "D",                0x64D2FF },
  { "T-shirt", "T",                0x30D158 },
};

const StyleOption MOODS[] = {
  { "Calm",    "~",               0x64D2FF },
  { "Happy",   ":)",              0x30D158 },
  { "Excited", "!",               0xFFD60A },
  { "Waiting", LV_SYMBOL_REFRESH, 0x0A84FF },
  { "Sleepy",  "zZ",              0xC4B5FD },
  { "Sad",     ":(",              0xFF453A },
};

constexpr uint8_t MAX_OPTIONS = 7;
constexpr int32_t ROW_W = 184;
constexpr int32_t ROW_H = 42;
constexpr int32_t ROW_GAP = 6;
constexpr int32_t ROW_PITCH = ROW_H + ROW_GAP;
constexpr int32_t STYLE_LIST_H = 150;
constexpr int32_t LIST_PAD = (STYLE_LIST_H - ROW_H) / 2;

lv_obj_t* styleScr;
lv_obj_t* titleLabel;
lv_obj_t* counter;
lv_obj_t* list;
lv_obj_t* row[MAX_OPTIONS];
lv_obj_t* rowChip[MAX_OPTIONS];
lv_obj_t* rowGlyph[MAX_OPTIONS];
lv_obj_t* rowName[MAX_OPTIONS];
lv_obj_t* rowCheck[MAX_OPTIONS];
StylePicker picker = StylePicker::Clothes;
uint8_t selected = 0;
uint8_t current = 0;

uint8_t optionCount() {
  return static_cast<uint8_t>(picker == StylePicker::Clothes ? kItemCount : kMoodCount);
}

const StyleOption* options() {
  return picker == StylePicker::Clothes ? CLOTHES : MOODS;
}

void counterFill(uint8_t i) {
  if (!counter) return;
  char buf[20];
  snprintf(buf, sizeof(buf), "%u of %u", static_cast<unsigned>(i + 1),
           static_cast<unsigned>(optionCount()));
  lv_label_set_text(counter, buf);
}

void rowPaint(uint8_t i, bool focused) {
  const StyleOption& option = options()[i];
  lv_obj_set_style_text_color(rowName[i], focused ? UI_COL_ON_PRIMARY : UI_COL_TEXT, 0);
  lv_obj_set_style_text_color(rowCheck[i], focused ? UI_COL_ON_PRIMARY : UI_COL_SUCCESS, 0);
  lv_obj_set_style_bg_color(rowChip[i], focused ? UI_COL_ON_PRIMARY : lv_color_hex(option.accent), 0);
  lv_obj_set_style_bg_opa(rowChip[i], focused ? LV_OPA_COVER : LV_OPA_70, 0);
  lv_obj_set_style_text_color(rowGlyph[i], focused ? UI_COL_PRIMARY : UI_COL_ON_PRIMARY, 0);
  lv_obj_set_style_text_opa(rowName[i], focused ? LV_OPA_COVER : LV_OPA_80, 0);
}

void rowFocused(lv_event_t* e) {
  lv_obj_t* target = lv_event_get_target_obj(e);
  uint8_t i = static_cast<uint8_t>(
      reinterpret_cast<intptr_t>(lv_obj_get_user_data(target)));
  selected = i;
  lv_obj_scroll_to_y(list, static_cast<int32_t>(i) * ROW_PITCH, LV_ANIM_ON);
  rowPaint(i, true);
  counterFill(i);
  uiThemeNudge(rowGlyph[i], -2);
}

void rowDefocused(lv_event_t* e) {
  lv_obj_t* target = lv_event_get_target_obj(e);
  uint8_t i = static_cast<uint8_t>(
      reinterpret_cast<intptr_t>(lv_obj_get_user_data(target)));
  rowPaint(i, false);
}

lv_obj_t* buildStyleScreen() {
  styleScr = lv_obj_create(nullptr);
  uiThemeScreen(styleScr);

  titleLabel = uiThemeLabel(styleScr, &lv_font_montserrat_16, UI_COL_TEXT);
  lv_label_set_text(titleLabel, picker == StylePicker::Clothes ? "Clothes" : "Mood");
  lv_obj_align(titleLabel, LV_ALIGN_TOP_MID, 0, 10);

  counter = uiThemeLabel(styleScr, &lv_font_montserrat_12, UI_COL_MUTED);
  lv_obj_align(counter, LV_ALIGN_TOP_MID, 0, 30);

  list = lv_obj_create(styleScr);
  lv_obj_set_size(list, ROW_W + 8, STYLE_LIST_H);
  lv_obj_align(list, LV_ALIGN_CENTER, 0, 4);
  lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(list, 0, 0);
  lv_obj_set_style_pad_hor(list, 4, 0);
  lv_obj_set_style_pad_ver(list, LIST_PAD, 0);
  lv_obj_set_style_pad_row(list, ROW_GAP, 0);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_scroll_elastic(list, false);

  for (uint8_t i = 0; i < MAX_OPTIONS; i++) {
    row[i] = lv_obj_create(list);
    lv_obj_set_size(row[i], ROW_W, ROW_H);
    lv_obj_set_scroll_on_focus(row[i], false);
    uiThemeGlass(row[i], true);
    lv_obj_set_style_radius(row[i], UI_RADIUS_CARD, 0);
    lv_obj_set_style_bg_color(row[i], UI_COL_PRIMARY, LV_STATE_FOCUSED);
    lv_obj_set_style_bg_opa(row[i], LV_OPA_COVER, LV_STATE_FOCUSED);
    lv_obj_set_user_data(row[i], reinterpret_cast<void*>(static_cast<intptr_t>(i)));
    lv_obj_add_event_cb(row[i], rowFocused, LV_EVENT_FOCUSED, nullptr);
    lv_obj_add_event_cb(row[i], rowDefocused, LV_EVENT_DEFOCUSED, nullptr);

    rowChip[i] = lv_obj_create(row[i]);
    lv_obj_set_size(rowChip[i], 30, 30);
    lv_obj_align(rowChip[i], LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_radius(rowChip[i], UI_RADIUS_SMALL, 0);
    lv_obj_set_style_border_width(rowChip[i], 0, 0);
    lv_obj_set_style_pad_all(rowChip[i], 0, 0);
    lv_obj_set_scrollable(rowChip[i], false);

    rowGlyph[i] = uiThemeLabel(rowChip[i], &lv_font_montserrat_14, UI_COL_TEXT);
    lv_obj_center(rowGlyph[i]);

    rowName[i] = uiThemeLabel(row[i], &lv_font_montserrat_16, UI_COL_TEXT);
    uiThemeTextBounds(rowName[i], 104, 20);
    lv_obj_align(rowName[i], LV_ALIGN_LEFT_MID, 46, 0);

    rowCheck[i] = uiThemeLabel(row[i], &lv_font_montserrat_14, UI_COL_SUCCESS);
    lv_label_set_text(rowCheck[i], LV_SYMBOL_OK);
    lv_obj_align(rowCheck[i], LV_ALIGN_RIGHT_MID, -12, 0);
  }

  uiThemeControlHint(styleScr, "choose");
  return styleScr;
}

} // namespace

void uiScreenStyleShow(StylePicker kind) {
  picker = kind;
  if (picker == StylePicker::Clothes) current = static_cast<uint8_t>(g_ui.pet->item());
  else current = static_cast<uint8_t>(g_ui.pet->mood());
  selected = current < optionCount() ? current : 0;

  bool freeMenu = g_ui.menuScreen && lv_screen_active() == g_ui.menuScreen;
  if (freeMenu) uiScreenMenuShrink();
  if (!styleScr) g_ui.styleScreen = buildStyleScreen();

  const StyleOption* defs = options();
  for (uint8_t i = 0; i < MAX_OPTIONS; i++) {
    bool visible = i < optionCount();
    lv_obj_set_hidden(row[i], !visible);
    if (!visible) continue;
    lv_label_set_text(rowGlyph[i], defs[i].glyph);
    lv_label_set_text(rowName[i], defs[i].name);
    lv_obj_set_hidden(rowCheck[i], i != current);
    rowPaint(i, false);
  }

  const uint8_t focus = selected;
  uiGroupSet(row, optionCount(), false);
  lv_group_focus_obj(row[focus]);
  lv_obj_scroll_to_y(list, static_cast<int32_t>(focus) * ROW_PITCH, LV_ANIM_OFF);
  counterFill(focus);
  lv_screen_load_anim(styleScr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 160, 0, freeMenu);
  if (freeMenu) uiScreenMenuFreed();
}

void uiScreenStyleFreed() {
  for (uint8_t i = 0; i < MAX_OPTIONS; i++) {
    if (row[i]) lv_anim_delete(row[i], nullptr);
    if (rowGlyph[i]) lv_anim_delete(rowGlyph[i], nullptr);
    row[i] = nullptr;
    rowChip[i] = nullptr;
    rowGlyph[i] = nullptr;
    rowName[i] = nullptr;
    rowCheck[i] = nullptr;
  }
  styleScr = nullptr;
  titleLabel = nullptr;
  counter = nullptr;
  list = nullptr;
  g_ui.styleScreen = nullptr;
}

bool uiScreenStyleActive() {
  return styleScr && lv_screen_active() == styleScr;
}

void uiScreenStyleNext() {
  if (!styleScr) return;
  selected = (selected + 1) % optionCount();
  lv_group_focus_obj(row[selected]);
}

void uiScreenStylePrev() {
  if (!styleScr) return;
  selected = (selected + optionCount() - 1) % optionCount();
  lv_group_focus_obj(row[selected]);
}

void uiScreenStyleActivate(float nowSec) {
  if (!styleScr) return;
  const char* name = options()[selected].name;
  if (picker == StylePicker::Clothes) {
    if (g_ui.actions.selectItem) g_ui.actions.selectItem(selected);
  } else {
    if (g_ui.actions.selectMood) g_ui.actions.selectMood(selected, nowSec);
  }
  feedback::chime();
  char msg[32];
  snprintf(msg, sizeof(msg), "%s selected", name);
  uiScreenPetToast(msg, false);
  uiScreenPetShow();
}

void uiScreenStyleBack() {
  if (!styleScr) return;
  uiScreenMenuShow();
}
