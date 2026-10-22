/*
  screen_menu.cpp — LVGL menu screen (third screen in the pet -> portfolio
  -> menu cycle): a vertical, scrollable launcher with TWO PAGES.

    APPS page:        [+] Feed pet   [=] Portfolio   [~] Cosmetics   [] Open DCA
    COSMETICS page:   [] Accessory   :) Mood         [~] Backdrop   [.] Parental

  - each row: circular icon chip + app name; rows scroll vertically, the
    focused row gets the light-blue ring + stronger glass tint and is
    auto-scrolled into view (lv_obj_scroll_to_view)
  - reserved right zone: on entry a BACK affordance lives there (focused
    first). On the APPS page it returns to the pet screen; on the COSMETICS
    page it steps back to the APPS page. Once the user walks into the list,
    the zone shows a short muted info word for the focused row instead
    ("happy +5", the current accessory/mood/backdrop name, "SOL->USDC")
  - Portfolio is the on-device DCA selection: it jumps straight to the
    portfolio screen. Open DCA stays the app-side request (dca:req).
  - BTN short (either button) walks the focus down the rows; past the last
    row uiScreenMenuFocusAdvance returns false and input.cpp moves on to
    the pet screen (contract unchanged). BTN long (either button) runs the
    focused row with the 160 ms press pulse.

  Pages reuse the same four row objects (restyled on switch) — the screen
  is built lazily on first show and freed on exit, and the page switch
  costs zero heap. The focus group only ever holds the visible objects
  (uiGroupSet discipline). The BLE/state side of every action lives in
  main.cpp (ui::Actions); this file owns only the widgets and the walking.
*/

#include "ui_internal.h"

namespace {

constexpr uint8_t PAGE_APPS = 0;
constexpr uint8_t PAGE_COSMETICS = 1;
constexpr uint8_t ROW_COUNT = 4;   // max rows on either page

struct RowDef { const char* name; const char* glyph; bool primary; };
const RowDef APP_ROWS[ROW_COUNT] = {
  { "Feed pet",  LV_SYMBOL_PLUS,  true  },   // primary action
  { "Portfolio", LV_SYMBOL_LIST,  false },   // on-device DCA selection
  { "Cosmetics", LV_SYMBOL_EDIT,  false },   // -> COSMETICS page
  { "Open DCA",  LV_SYMBOL_BARS,  false },   // app-side request (dca:req)
};
const RowDef COSMETIC_ROWS[ROW_COUNT] = {
  { "Accessory", LV_SYMBOL_IMAGE,     false },
  { "Mood",      ":)",                false },
  { "Backdrop",  LV_SYMBOL_TINT,      false },
  { "Parental",  LV_SYMBOL_EYE_CLOSE, false },   // optional code lock
};
uint8_t rowCountFor(uint8_t) { return ROW_COUNT; }   // both pages are full

uint8_t page = PAGE_APPS;

// Current accessory/mood/backdrop names, mirrored from main.cpp
// (ui::setMenu*) — shown as the focused row's info word in the right zone.
char accessoryName[12] = "none";
char moodName[12] = "calm";
char backdropName[12] = "default";
char lockName[8] = "off";

lv_obj_t* menuScr;
lv_obj_t* list;
lv_obj_t* row[ROW_COUNT];
lv_obj_t* rowChip[ROW_COUNT];
lv_obj_t* rowGlyph[ROW_COUNT];
lv_obj_t* rowName[ROW_COUNT];
lv_obj_t* backArrow;    // right-zone back affordance (visible on entry)
lv_obj_t* infoLabel;    // right-zone per-row info (visible otherwise)

const lv_font_t* CHIP_FONT = &lv_font_montserrat_16;

lv_obj_t* focusedRow() {
  return lv_group_get_focused(g_ui.group);
}

// Right-zone info for the focused row: a short state word, not a hint.
void infoFill() {
  lv_obj_t* f = focusedRow();
  const char* text = "";
  if (page == PAGE_APPS) {
    if (f == row[0])      text = "happy +5";
    else if (f == row[1]) text = "plans";
    else if (f == row[2]) text = accessoryName;
    else if (f == row[3]) text = "SOL->USDC";
  } else {
    if (f == row[0])      text = accessoryName;
    else if (f == row[1]) text = moodName;
    else if (f == row[2]) text = backdropName;
    else if (f == row[3]) text = lockName;
  }
  lv_label_set_text(infoLabel, text);
  lv_obj_set_hidden(infoLabel, false);
}

void rowFocused(lv_event_t*) {
  lv_obj_t* f = focusedRow();
  if (f == backArrow) return;   // zone shows the arrow itself
  lv_obj_scroll_to_view(f, LV_ANIM_ON);
  infoFill();
}

// 160 ms, 0.97 scale pulse on activation (app's PressableScale).
void pressPulse(lv_obj_t* obj) {
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, obj);
  lv_anim_set_values(&a, 256, 248);
  lv_anim_set_duration(&a, 80);
  lv_anim_set_playback_duration(&a, 80);
  lv_anim_set_exec_cb(&a, [](void* o, int32_t v) {
    lv_obj_set_style_transform_scale(static_cast<lv_obj_t*>(o), v, 0);
  });
  lv_anim_start(&a);
}

void showPage(uint8_t p);   // defined below (used by activate/back)

void activate(uint8_t idx, float nowSec) {
  ui::Actions& a = g_ui.actions;
  if (page == PAGE_APPS) {
    // Financial rows are gated while the parental lock is engaged;
    // feed/cosmetics stay free. Unlocking returns to the pet screen, so
    // the row just run needs one more activation once unlocked.
    if ((idx == 1 || idx == 3) && g_ui.locked) { uiScreenLockShow(0); return; }
    switch (idx) {
      case 0: if (a.feedPet) a.feedPet(nowSec); break;
      case 1: uiScreenDcaShow(); break;          // DCA selection (on-device)
      case 2: showPage(PAGE_COSMETICS); break;
      case 3: if (a.openDca) a.openDca(); break;
    }
  } else {
    switch (idx) {
      case 0: if (a.cycleItem) a.cycleItem(); break;
      case 1: if (a.cycleMood) a.cycleMood(nowSec); break;
      case 2: if (a.cycleBackdrop) a.cycleBackdrop(); break;
      case 3: if (a.parentalLock) a.parentalLock(); break;
    }
  }
}

void rowClicked(lv_event_t* e) {
  // Keypad ENTER delivers LV_EVENT_CLICKED on the focused row.
  lv_obj_t* t = lv_event_get_target_obj(e);
  if (t == backArrow) {
    if (page == PAGE_COSMETICS) showPage(PAGE_APPS);
    else uiScreenPetShow();
    return;
  }
  uint8_t idx = static_cast<uint8_t>(
      reinterpret_cast<intptr_t>(lv_obj_get_user_data(t)));
  pressPulse(row[idx]);
  activate(idx, millis() / 1000.0f);
}

// Restyle a row for its page def (only Feed is primary-colored).
void applyRowStyle(uint8_t i, bool primary) {
  lv_obj_set_style_bg_color(rowChip[i], primary ? UI_COL_PRIMARY : UI_COL_SURFACE, 0);
  lv_obj_set_style_bg_opa(rowChip[i], primary ? LV_OPA_COVER : LV_OPA_40, 0);
  lv_obj_set_style_border_color(rowChip[i], primary ? UI_COL_PRIMARY : UI_COL_BORDER, 0);
  lv_obj_set_style_text_color(rowGlyph[i], primary ? UI_COL_ON_PRIMARY : UI_COL_TEXT, 0);
}

// Rebuild the rows for a page: restyle the shared row objects, refocus the
// back affordance, scroll to the top. No allocation — same widgets reused.
void showPage(uint8_t p) {
  page = p;
  const RowDef* defs = page == PAGE_APPS ? APP_ROWS : COSMETIC_ROWS;
  uint8_t n = rowCountFor(page);
  for (uint8_t i = 0; i < ROW_COUNT; i++) {
    bool used = i < n;
    lv_obj_set_hidden(row[i], !used);
    if (!used) continue;
    lv_label_set_text(rowName[i], defs[i].name);
    lv_label_set_text(rowGlyph[i], defs[i].glyph);
    applyRowStyle(i, defs[i].primary);
    lv_obj_set_user_data(row[i], reinterpret_cast<void*>(static_cast<intptr_t>(i)));
  }
  lv_obj_set_hidden(backArrow, false);
  lv_obj_set_hidden(infoLabel, true);
  lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
  lv_obj_t* visible[ROW_COUNT + 1];
  visible[0] = backArrow;
  for (uint8_t i = 0; i < n; i++) visible[i + 1] = row[i];
  uiGroupSet(visible, n + 1, false);   // focuses the arrow
}

} // namespace

// Internal builder: the widgets are allocated on first show (lazy) and
// freed again on exit — eager creation in ui::begin() exhausted the boot
// heap. Row names are cached file-statics, so a rebuild always reflects
// the latest BLE-pushed state.
lv_obj_t* buildMenuScreen() {
  menuScr = lv_obj_create(nullptr);
  uiThemeScreen(menuScr);

  // --- scrollable app list (left ~70%) ---
  // Inset from the top-left corner (the round panel clips it) and shorter
  // at the bottom; rows still scroll through the remaining edge zones.
  list = lv_obj_create(menuScr);
  lv_obj_set_size(list, 156, 190);
  lv_obj_align(list, LV_ALIGN_TOP_LEFT, 24, 30);
  lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(list, 0, 0);
  lv_obj_set_style_pad_all(list, 2, 0);
  lv_obj_set_style_pad_row(list, 10, 0);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_set_style_bg_color(list, UI_COL_PRIMARY, LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_SCROLLBAR);
  lv_obj_set_style_width(list, 3, LV_PART_SCROLLBAR);
  lv_obj_set_style_radius(list, 2, LV_PART_SCROLLBAR);
  lv_obj_set_style_pad_right(list, 6, 0);

  for (uint8_t i = 0; i < ROW_COUNT; i++) {
    row[i] = lv_obj_create(list);
    lv_obj_set_size(row[i], 140, 56);
    uiThemeGlass(row[i], true);
    lv_obj_set_style_radius(row[i], UI_RADIUS_CARD, 0);
    lv_obj_set_style_bg_color(row[i], UI_COL_SURFACE, LV_STATE_FOCUSED);
    lv_obj_set_style_bg_opa(row[i], LV_OPA_60, LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(row[i], 2, LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(row[i], UI_COL_PRIMARY, LV_STATE_FOCUSED);
    lv_obj_set_style_border_opa(row[i], LV_OPA_COVER, LV_STATE_FOCUSED);
    lv_obj_set_clickable(row[i], true);
    lv_obj_add_event_cb(row[i], rowClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(row[i], rowFocused, LV_EVENT_FOCUSED, nullptr);

    // Circular icon chip on the left (styled per page def in showPage).
    rowChip[i] = lv_obj_create(row[i]);
    lv_obj_set_size(rowChip[i], 38, 38);
    lv_obj_align(rowChip[i], LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_radius(rowChip[i], LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(rowChip[i], 1, 0);
    lv_obj_set_style_border_opa(rowChip[i], LV_OPA_30, 0);
    lv_obj_set_style_pad_all(rowChip[i], 0, 0);
    lv_obj_set_scrollable(rowChip[i], false);
    lv_obj_set_clickable(rowChip[i], false);

    rowGlyph[i] = uiThemeLabel(rowChip[i], CHIP_FONT, UI_COL_TEXT);
    lv_obj_center(rowGlyph[i]);

    rowName[i] = uiThemeLabel(row[i], &lv_font_montserrat_14, UI_COL_TEXT);
    lv_obj_align(rowName[i], LV_ALIGN_LEFT_MID, 54, 0);
  }

  // --- reserved right zone: back affordance + per-row info ---
  // Moved down/in from the old corner position — the round panel clipped
  // the chip's top-right edge there.
  backArrow = lv_obj_create(menuScr);
  lv_obj_set_size(backArrow, 40, 40);
  lv_obj_align(backArrow, LV_ALIGN_TOP_RIGHT, -20, 60);
  uiThemeGlass(backArrow, true);
  lv_obj_set_style_radius(backArrow, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(backArrow, 3, LV_STATE_FOCUSED);
  lv_obj_set_style_border_color(backArrow, UI_COL_PRIMARY, LV_STATE_FOCUSED);
  lv_obj_set_style_border_opa(backArrow, LV_OPA_COVER, LV_STATE_FOCUSED);
  lv_obj_set_clickable(backArrow, true);
  lv_obj_add_event_cb(backArrow, rowClicked, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* arrowGlyph = uiThemeLabel(backArrow, &lv_font_montserrat_20, UI_COL_PRIMARY);
  lv_label_set_text(arrowGlyph, LV_SYMBOL_RIGHT);
  lv_obj_center(arrowGlyph);

  infoLabel = uiThemeLabel(menuScr, &lv_font_montserrat_12, UI_COL_MUTED);
  lv_obj_align(infoLabel, LV_ALIGN_TOP_RIGHT, -20, 108);
  lv_obj_set_hidden(infoLabel, true);

  return menuScr;
}

void uiScreenMenuShow() {
  if (!menuScr) g_ui.menuScreen = buildMenuScreen();   // lazy build
  // Enter on the APPS page, at the top: the back affordance is focused
  // first (long-press/action = pet screen); the rows follow in the chain.
  showPage(PAGE_APPS);
  lv_screen_load_anim(menuScr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
}

void uiScreenMenuFreed() {
  // The screen object was deleted via the exit transition's auto_del; only
  // the pointers need clearing. Names stay cached in file-statics.
  menuScr = nullptr;
  g_ui.menuScreen = nullptr;
}

void uiScreenMenuBack() {
  if (!menuScr) return;
  if (page == PAGE_COSMETICS) showPage(PAGE_APPS);   // step back a page
  else uiScreenPetShow();
}

void uiScreenMenuActivate(float nowSec) {
  if (!menuScr) return;
  lv_obj_t* f = focusedRow();
  if (!f) return;
  if (f == backArrow) {
    if (page == PAGE_COSMETICS) showPage(PAGE_APPS);
    else uiScreenPetShow();
    return;
  }
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
    infoFill();
    return true;
  }
  if (f == backArrow) {
    // Walk into the list: the back affordance leaves the focus group and
    // the right zone switches to per-row info until the next entry.
    lv_obj_set_hidden(backArrow, true);
    lv_obj_t* visible[ROW_COUNT];
    uint8_t n = rowCountFor(page);
    for (uint8_t i = 0; i < n; i++) visible[i] = row[i];
    uiGroupSet(visible, n, false);   // focuses row[0]
    lv_obj_scroll_to_view(row[0], LV_ANIM_ON);
    infoFill();
    return true;
  }
  if (f == row[rowCountFor(page) - 1]) return false;   // past the end: pet screen
  lv_group_focus_next(g_ui.group);
  return true;   // rowFocused scrolls it into view + updates the info
}

// Crown CCW (PCB v1) = previous row. At the top of the list (or on the
// back arrow) the caller backs out via uiScreenMenuBack.
bool uiScreenMenuFocusPrev() {
  if (!menuScr) return false;
  lv_obj_t* f = focusedRow();
  if (!f) {
    lv_group_focus_obj(row[rowCountFor(page) - 1]);
    infoFill();
    return true;
  }
  if (f == backArrow) return false;
  if (f == row[0]) return false;
  lv_group_focus_prev(g_ui.group);
  return true;
}

// Name caches are always written (BLE writes may arrive before the screen
// is ever built); the builder reads them on first show. When the screen
// exists and the info word is visible, just refresh it.
void uiScreenMenuSetAccessory(const char* name) {
  strlcpy(accessoryName, name, sizeof(accessoryName));
  if (!menuScr) return;
  if (!lv_obj_is_hidden(infoLabel)) infoFill();
}

void uiScreenMenuSetMood(const char* name) {
  strlcpy(moodName, name, sizeof(moodName));
  if (!menuScr) return;
  if (!lv_obj_is_hidden(infoLabel)) infoFill();
}

void uiScreenMenuSetBackdrop(const char* name) {
  strlcpy(backdropName, name, sizeof(backdropName));
  if (!menuScr) return;
  if (!lv_obj_is_hidden(infoLabel)) infoFill();
}

void uiScreenMenuSetLock(const char* name) {
  strlcpy(lockName, name, sizeof(lockName));
  if (!menuScr) return;
  if (!lv_obj_is_hidden(infoLabel)) infoFill();
}
