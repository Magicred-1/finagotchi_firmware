/*
  screen_create.cpp — "+ New plan" WHEEL: pick token / amount / frequency
  with vertical drums, like the app's wheel picker, fully standalone
  (browsing and composing need no app connection; only the final send does,
  which already degrades to a "connect the app" toast).

  Paged flow (same launcher-list idiom as the menu screen):
    TOKEN drum -> AMOUNT drum -> FREQUENCY drum -> SUMMARY/CREATE

  Each drum is a vertical scrollable list of option rows (clip top/bottom,
  slim cyan scrollbar on the right edge, focused row = cyan ring, focused
  row auto-scrolls into view). A step label on top names the drum
  ("TOKEN 1/3" ...); the right zone carries the menu's reserved-space
  pattern: on the TOKEN drum it holds the back-arrow affordance (activate =
  back to the portfolio), on later drums a muted info word with the values
  picked so far ("SPYX", "SPYX  0.05 SOL").

  Interaction (redundant two-button scheme — short = navigate, long = act):
    short = scroll to the next option (wraps at the end back to the first)
    long  = confirm this drum and advance to the next page
    summary view: short = back to edit (token drum), long = CREATE ->
                  Actions.createPlan (offline: toast + stay; connected:
                  dca:new: notify + toast + back to the portfolio)
  Back-out rule: the TOKEN drum is entered with the back arrow focused, so
  backing out is always one long-press away on page 1; deeper pages return
  to the token drum via the summary view's short press.
*/

#include "ui_internal.h"

namespace {

constexpr uint8_t PAGE_TOKEN = 0;
constexpr uint8_t PAGE_AMOUNT = 1;
constexpr uint8_t PAGE_FREQ = 2;
constexpr uint8_t PAGE_SUMMARY = 3;

const float AMOUNTS[] = { 0.01f, 0.05f, 0.1f, 0.25f };
constexpr uint8_t AMOUNT_COUNT = 4;
struct Freq { const char* name; uint32_t sec; char chip; };
const Freq FREQS[] = {
  { "daily", 86400, 'D' }, { "weekly", 604800, 'W' }, { "monthly", 2592000, 'M' }
};
constexpr uint8_t FREQ_COUNT = 3;

// Fallback list; main replaces it with the xStocks mint table tickers.
const char* const FALLBACK_TICKERS[] = { "SPYX", "GOOGLX", "HOODX" };
constexpr uint8_t MAX_OPTIONS = 13;   // == size of main's XSTOCK_MINTS table
const char* const* tickers = FALLBACK_TICKERS;
uint8_t tickerCount = 3;

lv_obj_t* createScr;
lv_obj_t* stepLabel;
lv_obj_t* infoLabel;      // right zone: picked values so far
lv_obj_t* backArrow;      // right zone, TOKEN drum only
lv_obj_t* list;           // drum list
lv_obj_t* opt[MAX_OPTIONS];
lv_obj_t* optLogo[MAX_OPTIONS];    // lv_image (tokens with a logo)
lv_obj_t* optMono[MAX_OPTIONS];    // monogram/glyph label (fallback, $, D/W/M)
lv_obj_t* optChip[MAX_OPTIONS];
lv_obj_t* optText[MAX_OPTIONS];

lv_obj_t* sumAmount;
lv_obj_t* sumPlan;
lv_obj_t* createRow;

uint8_t page = PAGE_TOKEN;
uint8_t optCount = 0;
uint8_t tokenIdx = 0, amountIdx = 1, freqIdx = 0;

lv_obj_t* focused() { return lv_group_get_focused(g_ui.group); }

void rowFocused(lv_event_t* e) {
  lv_obj_t* f = lv_event_get_target_obj(e);
  if (f != backArrow) lv_obj_scroll_to_view(f, LV_ANIM_ON);
}

// --- drums ---------------------------------------------------------------------

void infoFill() {
  char buf[28] = "";
  if (page == PAGE_AMOUNT) {
    strlcpy(buf, tickers[tokenIdx], sizeof(buf));
  } else if (page >= PAGE_FREQ) {
    snprintf(buf, sizeof(buf), "%s\n%.4g SOL", tickers[tokenIdx],
             static_cast<double>(AMOUNTS[amountIdx]));
  }
  lv_label_set_text(infoLabel, buf);
  lv_obj_set_hidden(infoLabel, page == PAGE_TOKEN);
}

// Monogram chip fallback for tickers without a logo (same rule as the
// portfolio cards).
void chipMonogram(uint8_t i, const char* ticker) {
  size_t tl = strlen(ticker);
  char init[4] = { 0, 0, 0, 0 };
  if (tl <= 3) strlcpy(init, ticker, sizeof(init));
  else { init[0] = ticker[0]; init[1] = ticker[1]; }
  lv_label_set_text(optMono[i], init);
  lv_obj_set_hidden(optMono[i], false);
  lv_obj_set_hidden(optLogo[i], true);
}

// Chip shows a token logo when one exists, else a monogram/glyph.
void chipSet(uint8_t i, const char* ticker, const char* glyph) {
  const lv_image_dsc_t* logo = ticker ? uiTokenLogoDsc(ticker) : nullptr;
  if (logo) {
    lv_image_set_src(optLogo[i], logo);
    lv_obj_set_hidden(optLogo[i], false);
    lv_obj_set_hidden(optMono[i], true);
  } else if (ticker) {
    chipMonogram(i, ticker);
  } else {
    lv_label_set_text(optMono[i], glyph);
    lv_obj_set_hidden(optMono[i], false);
    lv_obj_set_hidden(optLogo[i], true);
  }
}

void showPage(uint8_t p);   // defined below (used by confirm/summary)

void confirmFocused() {
  lv_obj_t* f = focused();
  if (!f) return;
  if (f == backArrow) {          // TOKEN drum back affordance
    uiScreenDcaShow();
    return;
  }
  if (f == createRow) {          // SUMMARY: CREATE ->
    if (g_ui.actions.createPlan &&
        g_ui.actions.createPlan(tickers[tokenIdx], AMOUNTS[amountIdx],
                                FREQS[freqIdx].sec)) {
      uiScreenDcaShow();         // request is out: back to the portfolio
    }
    return;
  }
  // Drum option: record the pick and advance to the next page.
  uint8_t idx = static_cast<uint8_t>(
      reinterpret_cast<intptr_t>(lv_obj_get_user_data(f)));
  if (page == PAGE_TOKEN) tokenIdx = idx;
  else if (page == PAGE_AMOUNT) amountIdx = idx;
  else if (page == PAGE_FREQ) freqIdx = idx;
  showPage(page + 1);
}

void buildDrum() {
  for (uint8_t i = 0; i < MAX_OPTIONS; i++) {
    if (i < optCount) {
      const char* text = "";
      char buf[16];
      switch (page) {
        case PAGE_TOKEN:
          text = tickers[i];
          chipSet(i, tickers[i], nullptr);
          break;
        case PAGE_AMOUNT:
          snprintf(buf, sizeof(buf), "%.4g SOL", static_cast<double>(AMOUNTS[i]));
          text = buf;
          chipSet(i, nullptr, "$");
          break;
        default: {   // PAGE_FREQ
          text = FREQS[i].name;
          char c[2] = { FREQS[i].chip, 0 };
          chipSet(i, nullptr, c);
          break;
        }
      }
      lv_label_set_text(optText[i], text);
      lv_obj_set_hidden(opt[i], false);
    } else {
      lv_obj_set_hidden(opt[i], true);
    }
  }

  // Focus group: exactly the visible rows (+ the back arrow on page 1).
  if (page == PAGE_TOKEN) {
    lv_obj_t* visible[MAX_OPTIONS + 1];
    visible[0] = backArrow;
    for (uint8_t i = 0; i < optCount; i++) visible[i + 1] = opt[i];
    lv_obj_set_hidden(backArrow, false);
    uiGroupSet(visible, optCount + 1, false);   // focuses the arrow
  } else {
    lv_obj_set_hidden(backArrow, true);
    uiGroupSet(opt, optCount, false);           // focuses the first option
  }
  lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
}

void showPage(uint8_t p) {
  page = p;
  bool summary = (page == PAGE_SUMMARY);
  lv_obj_set_hidden(list, summary);
  lv_obj_set_hidden(sumAmount, !summary);
  lv_obj_set_hidden(sumPlan, !summary);
  lv_obj_set_hidden(createRow, !summary);

  static const char* STEPS[] = { "TOKEN  1/3", "AMOUNT  2/3", "FREQUENCY  3/3", "SUMMARY" };
  lv_label_set_text(stepLabel, STEPS[page]);

  if (summary) {
    char buf[20];
    snprintf(buf, sizeof(buf), "%.4g SOL", static_cast<double>(AMOUNTS[amountIdx]));
    lv_label_set_text(sumAmount, buf);
    snprintf(buf, sizeof(buf), "%s  %s", tickers[tokenIdx], FREQS[freqIdx].name);
    lv_label_set_text(sumPlan, buf);
    lv_obj_set_hidden(backArrow, true);
    lv_obj_set_hidden(infoLabel, true);
    lv_obj_t* one[1] = { createRow };
    uiGroupSet(one, 1, false);
    return;
  }

  optCount = (page == PAGE_TOKEN) ? tickerCount
           : (page == PAGE_AMOUNT) ? AMOUNT_COUNT : FREQ_COUNT;
  infoFill();
  buildDrum();
}

} // namespace

lv_obj_t* uiScreenCreateCreate() {
  createScr = lv_obj_create(nullptr);
  uiThemeScreen(createScr);

  stepLabel = uiThemeLabel(createScr, &lv_font_montserrat_12, UI_COL_MUTED);
  lv_obj_align(stepLabel, LV_ALIGN_TOP_MID, 0, 16);

  // --- drum list (launcher idiom: clip edges, slim cyan scrollbar) ---
  list = lv_obj_create(createScr);
  lv_obj_set_size(list, 160, 184);
  lv_obj_align(list, LV_ALIGN_TOP_LEFT, 16, 38);
  lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(list, 0, 0);
  lv_obj_set_style_pad_all(list, 2, 0);
  lv_obj_set_style_pad_row(list, 8, 0);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_set_style_bg_color(list, UI_COL_PRIMARY, LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_SCROLLBAR);
  lv_obj_set_style_width(list, 3, LV_PART_SCROLLBAR);
  lv_obj_set_style_radius(list, 2, LV_PART_SCROLLBAR);
  lv_obj_set_style_pad_right(list, 6, 0);

  for (uint8_t i = 0; i < MAX_OPTIONS; i++) {
    opt[i] = lv_obj_create(list);
    lv_obj_set_size(opt[i], 146, 40);
    lv_obj_set_style_bg_color(opt[i], UI_COL_SURFACE, 0);
    lv_obj_set_style_bg_color(opt[i], UI_COL_SURFACE_HI, LV_STATE_FOCUSED);
    lv_obj_set_style_bg_opa(opt[i], LV_OPA_COVER, 0);
    lv_obj_set_style_radius(opt[i], UI_RADIUS_SMALL, 0);
    lv_obj_set_style_border_width(opt[i], 1, 0);
    lv_obj_set_style_border_color(opt[i], UI_COL_BORDER, 0);
    lv_obj_set_style_border_width(opt[i], 2, LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(opt[i], UI_COL_PRIMARY, LV_STATE_FOCUSED);
    lv_obj_set_style_pad_all(opt[i], 0, 0);
    lv_obj_set_scrollable(opt[i], false);
    lv_obj_set_clickable(opt[i], true);
    lv_obj_set_user_data(opt[i], reinterpret_cast<void*>(static_cast<intptr_t>(i)));
    lv_obj_add_event_cb(opt[i], rowFocused, LV_EVENT_FOCUSED, nullptr);

    optChip[i] = lv_obj_create(opt[i]);
    lv_obj_set_size(optChip[i], 28, 28);
    lv_obj_align(optChip[i], LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_set_style_radius(optChip[i], LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(optChip[i], LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(optChip[i], UI_COL_SURFACE_HI, 0);
    lv_obj_set_style_border_width(optChip[i], 1, 0);
    lv_obj_set_style_border_color(optChip[i], UI_COL_BORDER, 0);
    lv_obj_set_style_pad_all(optChip[i], 0, 0);
    lv_obj_set_scrollable(optChip[i], false);
    lv_obj_set_clickable(optChip[i], false);

    // Token rows show the logo (optLogo) or a monogram/glyph (optMono).
    optLogo[i] = lv_image_create(optChip[i]);
    lv_obj_center(optLogo[i]);
    optMono[i] = uiThemeLabel(optChip[i], &lv_font_montserrat_12, UI_COL_TEXT);
    lv_obj_center(optMono[i]);

    optText[i] = uiThemeLabel(opt[i], &lv_font_montserrat_14, UI_COL_TEXT);
    lv_obj_align(optText[i], LV_ALIGN_LEFT_MID, 42, 0);
  }

  // --- right zone: back arrow (TOKEN drum) + picked-values info ---
  backArrow = lv_obj_create(createScr);
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
  lv_obj_add_event_cb(backArrow, rowFocused, LV_EVENT_FOCUSED, nullptr);
  lv_obj_t* arrowGlyph = uiThemeLabel(backArrow, &lv_font_montserrat_20, UI_COL_PRIMARY);
  lv_label_set_text(arrowGlyph, LV_SYMBOL_RIGHT);
  lv_obj_center(arrowGlyph);

  infoLabel = uiThemeLabel(createScr, &lv_font_montserrat_12, UI_COL_MUTED);
  lv_obj_set_style_text_align(infoLabel, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_align(infoLabel, LV_ALIGN_TOP_RIGHT, -14, 104);
  lv_obj_set_hidden(infoLabel, true);

  // --- summary view ---
  sumAmount = uiThemeLabel(createScr, &lv_font_montserrat_28, UI_COL_TEXT);
  lv_obj_align(sumAmount, LV_ALIGN_CENTER, 0, -32);
  sumPlan = uiThemeLabel(createScr, &lv_font_montserrat_16, UI_COL_MUTED);
  lv_obj_align(sumPlan, LV_ALIGN_CENTER, 0, 0);

  createRow = lv_obj_create(createScr);
  lv_obj_set_size(createRow, 150, 40);
  lv_obj_align(createRow, LV_ALIGN_CENTER, 0, 56);
  lv_obj_set_style_bg_color(createRow, UI_COL_PRIMARY, 0);
  lv_obj_set_style_bg_opa(createRow, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(createRow, UI_RADIUS_PILL, 0);
  lv_obj_set_style_border_width(createRow, 3, LV_STATE_FOCUSED);
  lv_obj_set_style_border_color(createRow, UI_COL_TEXT, LV_STATE_FOCUSED);
  lv_obj_set_style_pad_all(createRow, 0, 0);
  lv_obj_set_scrollable(createRow, false);
  lv_obj_set_clickable(createRow, true);
  lv_obj_add_event_cb(createRow, rowFocused, LV_EVENT_FOCUSED, nullptr);
  lv_obj_t* crLabel = uiThemeLabel(createRow, &lv_font_montserrat_16, UI_COL_BG);
  lv_label_set_text(crLabel, "CREATE " LV_SYMBOL_RIGHT);
  lv_obj_center(crLabel);

  return createScr;
}

void uiScreenCreateSetTickers(const char* const* list, uint8_t n) {
  if (!list || n == 0) return;
  tickers = list;
  tickerCount = n > MAX_OPTIONS ? MAX_OPTIONS : n;
  if (tokenIdx >= tickerCount) tokenIdx = 0;
}

void uiScreenCreateShow() {
  showPage(PAGE_TOKEN);
  lv_screen_load_anim(createScr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
}

// Short press (either button) = navigate.
void uiScreenCreateFocusAdvance() {
  if (page == PAGE_SUMMARY) {
    showPage(PAGE_TOKEN);   // back to edit, from the top
    return;
  }
  lv_obj_t* f = focused();
  if (!f || f == backArrow) {
    // Walk into the drum: the back arrow leaves the focus group.
    if (f == backArrow) lv_obj_set_hidden(backArrow, true);
    uiGroupSet(opt, optCount, false);   // focuses the first option
    lv_obj_scroll_to_view(opt[0], LV_ANIM_ON);
    return;
  }
  // Next option, wrapping at the end back to the first (wheel behavior).
  if (f == opt[optCount - 1]) lv_group_focus_obj(opt[0]);
  else lv_group_focus_next(g_ui.group);
}

// Long press (either button) = confirm drum / CREATE.
void uiScreenCreateActivate(float nowSec) {
  (void)nowSec;
  confirmFocused();
}
