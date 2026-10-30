/*
  screen_menu.cpp — one scrollable launcher with bounded per-row state.
  Feed, Play, Portfolio, Units, cosmetics, Wi-Fi, Update and Code lock all
  use the same focus/confirm/back semantics. Play and Units replace hidden
  double-press extras with visible actions.

  Focused rows scroll into view and pop gently; each row keeps its name and
  state in separate bounded labels. A footer shows the active board's
  controls. The screen is built lazily and freed on exit to protect the heap.
  The focus group holds visible rows only (uiGroupSet); BLE/state actions
  live in main.cpp (ui::Actions).
*/

#include "ui_internal.h"
#include "../version.h"   // FW_VERSION for the Update row's info word

namespace {

constexpr uint8_t ROW_COUNT = 10;
constexpr uint8_t ROW_FEED = 0;
constexpr uint8_t ROW_PLAY = 1;
constexpr uint8_t ROW_PORTFOLIO = 2;
constexpr uint8_t ROW_UNITS = 3;
constexpr uint8_t ROW_ACCESSORY = 4;
constexpr uint8_t ROW_MOOD = 5;
constexpr uint8_t ROW_BACKDROP = 6;
constexpr uint8_t ROW_WIFI = 7;
constexpr uint8_t ROW_UPDATE = 8;
constexpr uint8_t ROW_LOCK = 9;

struct RowDef { const char* name; const char* glyph; bool primary; };
const RowDef ROWS[ROW_COUNT] = {
  { "Feed pet",  LV_SYMBOL_PLUS,      true  },
  { "Play",      LV_SYMBOL_PLAY,      false },
  { "Portfolio", LV_SYMBOL_LIST,      false },
  { "Units",     LV_SYMBOL_REFRESH,   false },
  { "Accessory", LV_SYMBOL_IMAGE,     false },
  { "Mood",      ":)",                false },
  { "Backdrop",  LV_SYMBOL_TINT,      false },
  { "Wi-Fi",     LV_SYMBOL_WIFI,      false },
  { "Update",    LV_SYMBOL_DOWNLOAD,  false },
  { "Code lock", LV_SYMBOL_EYE_CLOSE, false },
};

char accessoryName[12] = "none";
char moodName[12] = "calm";
char backdropName[12] = "default";
char lockName[8] = "off";
char wifiName[33] = "not set";
char fwInfo[12];

lv_obj_t* menuScr;
lv_obj_t* list;
lv_obj_t* counter;      // "3 / 10" position under the title
lv_obj_t* row[ROW_COUNT];
lv_obj_t* rowChip[ROW_COUNT];
lv_obj_t* rowGlyph[ROW_COUNT];
lv_obj_t* rowName[ROW_COUNT];
lv_obj_t* rowInfo[ROW_COUNT];

const lv_font_t* CHIP_FONT = &lv_font_montserrat_16;

// Round-panel carousel: the focused bubble always sits on the screen's
// horizontal center line (the widest chord of the circle), with the previous
// and next bubbles peeking above and below. The list's top/bottom padding
// lets the first and last rows reach the center too. Geometry (240 px
// circle): list spans y 44..200, neighbour pills reach y 48 / 196 where the
// chord is ~186 px, and the pills' rounded ends keep them inside it.
constexpr int32_t ROW_W     = 188;
constexpr int32_t ROW_H     = 44;
constexpr int32_t ROW_GAP   = 8;
constexpr int32_t ROW_PITCH = ROW_H + ROW_GAP;
constexpr int32_t MENU_LIST_H    = 156;   // focused row + both neighbours
constexpr int32_t MENU_LIST_PAD  = (MENU_LIST_H - ROW_H) / 2;
// Text zones inside a row: chip 8..36, name 42..(42+NAME_W), info ends 12 px
// from the right edge. NAME_W + INFO_W + 42 + 12 + 4 (gap) == ROW_W, and both
// are sized for the longest strings in Montserrat 14 / 12 (tools-checked).
constexpr int32_t ROW_NAME_W = 72;
constexpr int32_t ROW_INFO_W = 58;

lv_obj_t* focusedRow() {
  return lv_group_get_focused(g_ui.group);
}

void infoFill() {
  if (!list) return;
  for (uint8_t i = 0; i < ROW_COUNT; i++) {
    const char* text = "";
    if (i == ROW_FEED)           text = "+5";
    else if (i == ROW_PLAY)      text = "react";
    else if (i == ROW_PORTFOLIO) text = "plans";
    else if (i == ROW_UNITS)     text = g_ui.amountInSol ? "SOL" : "USD";
    else if (i == ROW_ACCESSORY) text = accessoryName;
    else if (i == ROW_MOOD)      text = moodName;
    else if (i == ROW_BACKDROP)  text = backdropName;
    else if (i == ROW_WIFI)      text = wifiName;
    else if (i == ROW_UPDATE)    text = fwInfo;
    else if (i == ROW_LOCK)      text = lockName;
    lv_label_set_text(rowInfo[i], text);
  }
}

// Focused bubble lights up solid; children need manual color swaps (LV_STATE
// propagation stops at the row itself).
void rowPaint(uint8_t i, bool focused) {
  lv_obj_set_style_text_color(rowName[i], focused ? UI_COL_ON_PRIMARY : UI_COL_TEXT, 0);
  lv_obj_set_style_text_color(rowInfo[i], focused ? UI_COL_ON_PRIMARY : UI_COL_MUTED, 0);
  // Focused: the bubble is solid primary, so the chip inverts to dark-on-light.
  lv_obj_set_style_bg_color(rowChip[i],
      focused ? UI_COL_ON_PRIMARY
              : (ROWS[i].primary ? UI_COL_PRIMARY : UI_COL_SURFACE), 0);
  lv_obj_set_style_bg_opa(rowChip[i],
      (focused || ROWS[i].primary) ? LV_OPA_COVER : LV_OPA_40, 0);
  lv_obj_set_style_text_color(rowGlyph[i],
      focused ? UI_COL_PRIMARY
              : (ROWS[i].primary ? UI_COL_ON_PRIMARY : UI_COL_TEXT), 0);
  // Neighbour rows recede so the focused one reads as "the" choice.
  lv_obj_set_style_text_opa(rowName[i], focused ? LV_OPA_COVER : LV_OPA_70, 0);
  lv_obj_set_style_text_opa(rowInfo[i], focused ? LV_OPA_COVER : LV_OPA_60, 0);
}

void counterFill(uint8_t i) {
  if (!counter) return;
  char buf[12];
  snprintf(buf, sizeof(buf), "%u / %u", static_cast<unsigned>(i + 1),
           static_cast<unsigned>(ROW_COUNT));
  lv_label_set_text(counter, buf);
}

void rowFocused(lv_event_t* e) {
  lv_obj_t* f = lv_event_get_target_obj(e);
  uint8_t i = static_cast<uint8_t>(
      reinterpret_cast<intptr_t>(lv_obj_get_user_data(f)));
  // Center the focused bubble (MENU_LIST_PAD makes row i's centered scroll
  // offset exactly i * ROW_PITCH). Rows have SCROLL_ON_FOCUS cleared, so
  // this is the only scroll animation — two competing ones made the list
  // jitter.
  lv_obj_scroll_to_y(list, static_cast<int32_t>(i) * ROW_PITCH, LV_ANIM_ON);
  rowPaint(i, true);
  counterFill(i);
  uiThemeFocusPop(f);
}

void rowDefocused(lv_event_t* e) {
  lv_obj_t* f = lv_event_get_target_obj(e);
  uint8_t i = static_cast<uint8_t>(
      reinterpret_cast<intptr_t>(lv_obj_get_user_data(f)));
  rowPaint(i, false);
}

void pressPulse(lv_obj_t* obj) {
  uiThemePress(obj);
}

void activate(uint8_t idx, float nowSec) {
  ui::Actions& a = g_ui.actions;
  if (idx == ROW_PORTFOLIO && g_ui.locked) { uiScreenLockShow(0); return; }
  switch (idx) {
    case ROW_FEED:      if (a.feedPet) a.feedPet(nowSec); break;
    case ROW_PLAY:      if (a.cycleReaction) a.cycleReaction(nowSec); break;
    case ROW_PORTFOLIO: uiScreenDcaShow(); break;
    case ROW_UNITS:
      if (g_ui.solUsd > 0.0f) {
        uiScreenDcaToggleAmountUnit();
        infoFill();
        uiThemeNudge(rowInfo[idx], g_ui.amountInSol ? -4 : 4);
      } else {
        ui::enqueueToast("SOL price unavailable");
      }
      break;
    case ROW_ACCESSORY: if (a.cycleItem) a.cycleItem(); break;
    case ROW_MOOD:      if (a.cycleMood) a.cycleMood(nowSec); break;
    case ROW_BACKDROP:  if (a.cycleBackdrop) a.cycleBackdrop(); break;
    case ROW_WIFI:      uiScreenConnectShow(); break;
    case ROW_UPDATE:    if (a.checkUpdate) a.checkUpdate(); break;
    case ROW_LOCK:      if (a.parentalLock) a.parentalLock(); break;
  }
}

void rowClicked(lv_event_t* e) {
  // Keypad ENTER delivers LV_EVENT_CLICKED on the focused row.
  lv_obj_t* t = lv_event_get_target_obj(e);
  uint8_t idx = static_cast<uint8_t>(
      reinterpret_cast<intptr_t>(lv_obj_get_user_data(t)));
  pressPulse(row[idx]);
  activate(idx, millis() / 1000.0f);
}

} // namespace

// Internal builder: the widgets are allocated on first show (lazy) and
// freed again on exit — eager creation in ui::begin() exhausted the boot
// heap. Row names are cached file-statics, so a rebuild always reflects
// the latest BLE-pushed state.
lv_obj_t* buildMenuScreen() {
  menuScr = lv_obj_create(nullptr);
  uiThemeScreen(menuScr);
  snprintf(fwInfo, sizeof(fwInfo), "v%s", FW_VERSION);

  lv_obj_t* title = uiThemeLabel(menuScr, &lv_font_montserrat_14, UI_COL_EYEBROW);
  lv_obj_set_style_text_letter_space(title, 2, 0);
  lv_label_set_text(title, "MENU");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

  counter = uiThemeLabel(menuScr, &lv_font_montserrat_12, UI_COL_MUTED);
  lv_obj_set_style_text_opa(counter, LV_OPA_70, 0);
  lv_obj_align(counter, LV_ALIGN_TOP_MID, 0, 28);

  list = lv_obj_create(menuScr);
  lv_obj_set_size(list, ROW_W + 8, MENU_LIST_H);
  lv_obj_align(list, LV_ALIGN_CENTER, 0, 2);
  lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(list, 0, 0);
  lv_obj_set_style_pad_hor(list, 4, 0);
  lv_obj_set_style_pad_ver(list, MENU_LIST_PAD, 0);
  lv_obj_set_style_pad_row(list, ROW_GAP, 0);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);
  // No scrollbar: on the round panel it would sit in the clipped edge; the
  // "n / 10" counter carries the position instead.
  lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_scroll_elastic(list, false);

  for (uint8_t i = 0; i < ROW_COUNT; i++) {
    row[i] = lv_obj_create(list);
    lv_obj_set_size(row[i], ROW_W, ROW_H);
    lv_obj_set_scroll_on_focus(row[i], false);   // rowFocused scrolls
    uiThemeGlass(row[i], true);
    lv_obj_set_style_radius(row[i], LV_RADIUS_CIRCLE, 0);   // bubble, not card
    lv_obj_set_style_bg_color(row[i], UI_COL_PRIMARY, LV_STATE_FOCUSED);
    lv_obj_set_style_bg_opa(row[i], LV_OPA_COVER, LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(row[i], 0, LV_STATE_FOCUSED);
    lv_obj_set_clickable(row[i], true);
    lv_obj_set_user_data(row[i], reinterpret_cast<void*>(static_cast<intptr_t>(i)));
    lv_obj_add_event_cb(row[i], rowClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(row[i], rowFocused, LV_EVENT_FOCUSED, nullptr);
    lv_obj_add_event_cb(row[i], rowDefocused, LV_EVENT_DEFOCUSED, nullptr);

    rowChip[i] = lv_obj_create(row[i]);
    lv_obj_set_size(rowChip[i], 28, 28);
    lv_obj_align(rowChip[i], LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_radius(rowChip[i], LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(rowChip[i], 1, 0);
    lv_obj_set_style_border_opa(rowChip[i], LV_OPA_30, 0);
    lv_obj_set_style_border_color(rowChip[i], UI_COL_BORDER, 0);
    lv_obj_set_style_pad_all(rowChip[i], 0, 0);
    lv_obj_set_scrollable(rowChip[i], false);
    lv_obj_set_clickable(rowChip[i], false);

    rowGlyph[i] = uiThemeLabel(rowChip[i], CHIP_FONT, UI_COL_TEXT);
    lv_label_set_text(rowGlyph[i], ROWS[i].glyph);
    lv_obj_center(rowGlyph[i]);

    // Name and info zones never overlap (see ROW_NAME_W / ROW_INFO_W).
    rowName[i] = uiThemeLabel(row[i], &lv_font_montserrat_14, UI_COL_TEXT);
    uiThemeTextBounds(rowName[i], ROW_NAME_W, 18);
    lv_label_set_text(rowName[i], ROWS[i].name);
    lv_obj_align(rowName[i], LV_ALIGN_LEFT_MID, 42, 0);

    rowInfo[i] = uiThemeLabel(row[i], &lv_font_montserrat_12, UI_COL_MUTED);
    uiThemeTextBounds(rowInfo[i], ROW_INFO_W, 16);
    lv_obj_set_style_text_align(rowInfo[i], LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(rowInfo[i], LV_ALIGN_RIGHT_MID, -12, 0);

    rowPaint(i, false);
  }

  uiThemeControlHint(menuScr, "select");
  infoFill();
  counterFill(0);
  return menuScr;
}

void uiScreenMenuShow() {
  bool freeDca = uiScreenDcaOnList();
  if (freeDca) uiScreenDcaShrinkList();
  uiScreenDcaReleaseDetail();
  if (!menuScr) g_ui.menuScreen = buildMenuScreen();
  uiGroupSet(row, ROW_COUNT, false);
  lv_obj_t* cur = lv_screen_active();
  bool freeOta = g_ui.otaScreen && cur == g_ui.otaScreen;
  bool freeConnect = g_ui.connectScreen && cur == g_ui.connectScreen;
  lv_screen_load_anim(menuScr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0,
                      freeDca || freeOta || freeConnect);
  if (freeDca) uiScreenDcaListFreed();
  if (freeOta) uiScreenOtaFreed();
  if (freeConnect) uiScreenConnectFreed();
}

void uiScreenMenuShrink() {
  if (!menuScr || !list) return;
  for (uint8_t i = 0; i < ROW_COUNT; i++) {
    lv_anim_delete(row[i], nullptr);
    lv_anim_delete(rowInfo[i], nullptr);
    rowInfo[i] = nullptr;
  }
  lv_obj_delete(list);
  list = nullptr;
  uiGroupSet(nullptr, 0, false);
}

void uiScreenMenuFreed() {
  for (uint8_t i = 0; i < ROW_COUNT; i++) {
    if (row[i]) lv_anim_delete(row[i], nullptr);
    if (rowInfo[i]) lv_anim_delete(rowInfo[i], nullptr);
    row[i] = nullptr;
    rowInfo[i] = nullptr;
  }
  menuScr = nullptr;
  list = nullptr;
  counter = nullptr;
  g_ui.menuScreen = nullptr;
}

void uiScreenMenuBack() {
  if (!menuScr) return;
  uiScreenPetShow();   // one flat list: back always lands on the pet screen
}

void uiScreenMenuActivate(float nowSec) {
  if (!menuScr) return;
  lv_obj_t* f = focusedRow();
  if (!f) return;
  uint8_t idx = static_cast<uint8_t>(
      reinterpret_cast<intptr_t>(lv_obj_get_user_data(f)));
  pressPulse(row[idx]);
  activate(idx, nowSec);
}

bool uiScreenMenuFocusAdvance() {
  if (!menuScr) return false;
  lv_obj_t* f = focusedRow();
  if (!f) {
    lv_group_focus_obj(row[0]);
    return true;
  }
  if (f == row[ROW_COUNT - 1]) return false;   // past the end: pet screen
  lv_group_focus_next(g_ui.group);
  return true;   // rowFocused scrolls it into view + updates the info
}

// Crown CCW (PCB v1) = previous row. At the top of the list the caller
// backs out via uiScreenMenuBack.
bool uiScreenMenuFocusPrev() {
  if (!menuScr) return false;
  lv_obj_t* f = focusedRow();
  if (!f) {
    lv_group_focus_obj(row[ROW_COUNT - 1]);
    return true;
  }
  if (f == row[0]) return false;
  lv_group_focus_prev(g_ui.group);
  return true;
}

// Name caches are always written (BLE writes may arrive before the screen
// is ever built); the builder reads them on first show. When the screen
// exists and the info word is visible, just refresh it.
void uiScreenMenuSetAccessory(const char* name) {
  strlcpy(accessoryName, name, sizeof(accessoryName));
  if (list) infoFill();
}

void uiScreenMenuSetMood(const char* name) {
  strlcpy(moodName, name, sizeof(moodName));
  if (list) infoFill();
}

void uiScreenMenuSetBackdrop(const char* name) {
  strlcpy(backdropName, name, sizeof(backdropName));
  if (list) infoFill();
}

void uiScreenMenuSetLock(const char* name) {
  strlcpy(lockName, name, sizeof(lockName));
  if (list) infoFill();
}

void uiScreenMenuSetWifi(const char* name) {
  strlcpy(wifiName, name, sizeof(wifiName));
  if (list) infoFill();
}
