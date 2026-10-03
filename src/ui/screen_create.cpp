/*
  screen_create.cpp — "+ New plan" create screen (from the trailing card on
  the portfolio screen): a small form for a new DCA plan, sent to the app as
  dca:new:<TICKER>:<amountSol>:<freqSec> — the app opens its DCA wizard
  prefilled so the user confirms+signs on the phone.

  Four focusable rows (same card style + cyan focus ring as the menu):
    Token      — cycles the known xStocks tickers (main's mint table)
    Amount     — presets 0.01 / 0.05 / 0.1 / 0.25 SOL
    Frequency  — daily / weekly / monthly (86400 / 604800 / 2592000 s)
    Create →   — sends the request (app connected) and pops back

  Interaction (hint-free, rows show name + current value):
    left short  = next field (wraps after "Create →")
    right short = cycle the focused field's value; on "Create →" = send
    either long = back to the pet screen (global escape)
*/

#include "ui_internal.h"

namespace {

constexpr uint8_t FIELD_TOKEN = 0;
constexpr uint8_t FIELD_AMOUNT = 1;
constexpr uint8_t FIELD_FREQ = 2;
constexpr uint8_t FIELD_SEND = 3;
constexpr uint8_t FIELD_COUNT = 4;

const float AMOUNTS[] = { 0.01f, 0.05f, 0.1f, 0.25f };
constexpr uint8_t AMOUNT_COUNT = 4;
struct Freq { const char* name; uint32_t sec; };
const Freq FREQS[] = { { "daily", 86400 }, { "weekly", 604800 }, { "monthly", 2592000 } };
constexpr uint8_t FREQ_COUNT = 3;

// Fallback list; main replaces it with the xStocks mint table tickers.
const char* const FALLBACK_TICKERS[] = { "SPYX", "GOOGLX", "HOODX" };
const char* const* tickers = FALLBACK_TICKERS;
uint8_t tickerCount = 3;

lv_obj_t* createScr;
lv_obj_t* row[FIELD_COUNT];
lv_obj_t* rowValue[FIELD_COUNT];   // right-side value (null on "Create →")

uint8_t tokenIdx = 0, amountIdx = 1, freqIdx = 0;

void valuesFill() {
  lv_label_set_text(rowValue[FIELD_TOKEN], tickers[tokenIdx]);
  char buf[16];
  snprintf(buf, sizeof(buf), "%.4g SOL", static_cast<double>(AMOUNTS[amountIdx]));
  lv_label_set_text(rowValue[FIELD_AMOUNT], buf);
  lv_label_set_text(rowValue[FIELD_FREQ], FREQS[freqIdx].name);
}

void rowClicked(lv_event_t* e) {
  // Keypad ENTER delivers LV_EVENT_CLICKED on the focused row.
  uint8_t idx = static_cast<uint8_t>(
      reinterpret_cast<intptr_t>(lv_obj_get_user_data(lv_event_get_target_obj(e))));
  if (idx == FIELD_SEND) {
    if (g_ui.actions.createPlan &&
        g_ui.actions.createPlan(tickers[tokenIdx], AMOUNTS[amountIdx],
                                FREQS[freqIdx].sec)) {
      uiScreenDcaShow();   // request is out: back to the portfolio
    }
    return;
  }
  if (idx == FIELD_TOKEN) tokenIdx = (tokenIdx + 1) % tickerCount;
  else if (idx == FIELD_AMOUNT) amountIdx = (amountIdx + 1) % AMOUNT_COUNT;
  else freqIdx = (freqIdx + 1) % FREQ_COUNT;
  valuesFill();
}

} // namespace

lv_obj_t* uiScreenCreateCreate() {
  createScr = lv_obj_create(nullptr);
  uiThemeScreen(createScr);

  lv_obj_t* title = uiThemeLabel(createScr, &lv_font_montserrat_12, UI_COL_MUTED);
  lv_label_set_text(title, "NEW PLAN");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);

  static const char* FIELD_NAMES[FIELD_COUNT] = {
    "Token", "Amount", "Frequency", "Create " LV_SYMBOL_RIGHT
  };
  for (uint8_t i = 0; i < FIELD_COUNT; i++) {
    row[i] = lv_obj_create(createScr);
    lv_obj_set_size(row[i], 190, 36);
    lv_obj_align(row[i], LV_ALIGN_TOP_MID, 0, 48 + i * 42);
    uiThemeCard(row[i]);
    lv_obj_set_style_radius(row[i], UI_RADIUS_SMALL, 0);
    lv_obj_set_clickable(row[i], true);
    lv_obj_set_user_data(row[i], reinterpret_cast<void*>(static_cast<intptr_t>(i)));
    lv_obj_add_event_cb(row[i], rowClicked, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* name = uiThemeLabel(row[i], &lv_font_montserrat_14,
                                  i == FIELD_SEND ? UI_COL_PRIMARY : UI_COL_TEXT);
    lv_label_set_text(name, FIELD_NAMES[i]);
    if (i == FIELD_SEND) {
      lv_obj_center(name);
      rowValue[i] = nullptr;
    } else {
      lv_obj_align(name, LV_ALIGN_LEFT_MID, UI_SP3, 0);
      rowValue[i] = uiThemeLabel(row[i], &lv_font_montserrat_12, UI_COL_MUTED);
      lv_obj_align(rowValue[i], LV_ALIGN_RIGHT_MID, -UI_SP3, 0);
    }
  }
  valuesFill();
  return createScr;
}

void uiScreenCreateSetTickers(const char* const* list, uint8_t n) {
  if (!list || n == 0) return;
  tickers = list;
  tickerCount = n;
  if (tokenIdx >= tickerCount) tokenIdx = 0;
  valuesFill();
}

void uiScreenCreateShow() {
  uiGroupSet(row, FIELD_COUNT, false);
  lv_screen_load_anim(createScr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
}

void uiScreenCreateFocusAdvance() {
  lv_group_focus_next(g_ui.group);   // wraps after "Create →" back to Token
}

void uiScreenCreateActivate(float nowSec) {
  (void)nowSec;
  lv_obj_t* f = lv_group_get_focused(g_ui.group);
  if (!f) return;
  lv_obj_send_event(f, LV_EVENT_CLICKED, nullptr);
}
