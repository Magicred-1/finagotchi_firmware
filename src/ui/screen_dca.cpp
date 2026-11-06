/*
  screen_dca.cpp — LVGL Portfolio/Positions screen (app design language):
  - header: total portfolio value = sum(holdings x price_usd) across plans,
    big USD number with a SOL equivalent under it when the SOL/USD rate is
    known (BLE solusd: / NVS / standalone price feed)
  - scrollable position cards with logo/monogram, bounded values, countdown
    and holdings/buy line; the menu's Units row selects USD or SOL
  - overdue plans get the warning-amber treatment
  - focused cards scroll into view and use the shared spring feedback
  - confirm opens/closes detail; detail confirm toggles pause/resume
  - empty state and board-specific control hints stay in reserved regions

  Heap discipline: the list screen is built LAZILY on first show and freed
  on exit (auto_del), like the menu and create screens. Transitions never
  hold two screens' widgets at once: entering the create wheel first frees
  the card column (uiScreenDcaShrinkList), and returning from it frees the
  drum before this list rebuilds (uiScreenCreateShrink). Every
  widget-touching entry point guards on listScr being non-null.
*/

#include "ui_internal.h"
#include "../token_logos.h"

namespace {

// --- token logos ---------------------------------------------------------------
// The token_logos.h arrays hold NATIVE (little-endian) RGB565 — verified
// against the source PNGs. LVGL renders into a big-endian (SWAPPED) draw
// buffer for the panel, so native descriptors let the blend unit do the
// byte swap. (Marking them SWAPPED would push bytes verbatim and swap the
// colors on the panel.)

struct LogoDsc { const char* ticker; lv_image_dsc_t dsc; };
LogoDsc logos[TOKEN_LOGO_COUNT];
bool logosBuilt = false;

void buildLogos() {
  if (logosBuilt) return;
  logosBuilt = true;
  for (size_t i = 0; i < TOKEN_LOGO_COUNT; i++) {
    logos[i].ticker = TOKEN_LOGOS[i].ticker;
    lv_image_dsc_t& d = logos[i].dsc;
    d.header.magic = LV_IMAGE_HEADER_MAGIC;
    d.header.cf = LV_COLOR_FORMAT_RGB565;
    d.header.flags = 0;
    d.header.w = TOKEN_LOGO_SIZE;
    d.header.h = TOKEN_LOGO_SIZE;
    d.header.stride = 0;   // 0 = w * bpp
    d.header.reserved_2 = 0;
    d.data_size = TOKEN_LOGO_SIZE * TOKEN_LOGO_SIZE * 2;
    d.data = reinterpret_cast<const uint8_t*>(TOKEN_LOGOS[i].data);
  }
}

const lv_image_dsc_t* logoFor(const char* ticker) {
  buildLogos();
  for (size_t i = 0; i < TOKEN_LOGO_COUNT; i++)
    if (strcmp(ticker, logos[i].ticker) == 0) return &logos[i].dsc;
  return nullptr;
}

} // namespace

// Shared with the create-plan wheel (screen_create.cpp): logo descriptor
// for a ticker, nullptr when unknown (caller falls back to a monogram).
const lv_image_dsc_t* uiTokenLogoDsc(const char* ticker) {
  return logoFor(ticker);
}

namespace {

// Monogram-chip color for tickers without a logo: brand colors for the
// tokens the app lists, otherwise a deterministic djb2-hashed hue (ported
// from the old pet.cpp chrome).
void hsv2rgb(float h, float s, float v, uint8_t& r, uint8_t& g, uint8_t& b) {
  float c = v * s;
  float x = c * (1.0f - fabsf(fmodf(h * 6.0f, 2.0f) - 1.0f));
  float m = v - c;
  float rr, gg, bb;
  int seg = static_cast<int>(h * 6.0f) % 6;
  switch (seg) {
    case 0: rr = c; gg = x; bb = 0; break;
    case 1: rr = x; gg = c; bb = c; break;
    case 2: rr = 0; gg = 0; bb = c; break;
    case 3: rr = 0; gg = x; bb = c; break;
    case 4: rr = 0; gg = 0; bb = x; break;
    default: rr = 0; gg = x; bb = x; break;
  }
  r = static_cast<uint8_t>((rr + m) * 255.0f);
  g = static_cast<uint8_t>((gg + m) * 255.0f);
  b = static_cast<uint8_t>((bb + m) * 255.0f);
}

lv_color_t tokenColor(const char* ticker) {
  struct Known { const char* sym; uint8_t r, g, b; };
  static const Known KNOWN[] = {
    // native / DeFi
    { "SOL",  153,  69, 255 },
    { "USDC",  39, 117, 202 },
    { "BONK", 253, 186,  39 },
    { "JUP",    0, 194, 139 },
    { "WIF",  124,  58, 237 },
    // xStocks
    { "SPYX",  225,  60,  57 },   // SPYx  — State Street red
    { "GOOGLX", 66, 133, 244 },   // GOOGLx — Google blue
    { "AAPLX", 162, 170, 173 },   // AAPLx — Apple silver
    { "TSLAX", 232,  33,  39 },   // TSLAx — Tesla red
    { "NVDAX", 118, 185,   0 },   // NVDAx — NVIDIA green
    { "METAX",   0, 129, 251 },   // METAx — Meta blue
    { "MSFTX",   0, 164, 239 },   // MSFTx — Microsoft blue
    { "AMZNX", 255, 153,   0 },   // AMZNx — Amazon orange
    { "MSTRX", 247, 147,  26 },   // MSTRx — Strategy orange
    { "CRCLX",  41,  98, 255 },   // CRCLx — Circle blue
    { "NFLXX", 229,   9,  20 },   // NFLXx — Netflix red
    { "COINX",   0,  82, 255 },   // COINx — Coinbase blue
    { "HOODX",   0, 200,   5 },   // HOODx — Robinhood green
  };
  for (const auto& k : KNOWN)
    if (strcmp(ticker, k.sym) == 0) return lv_color_make(k.r, k.g, k.b);
  uint32_t h = 5381;
  for (const char* c = ticker; *c; c++) h = h * 33 + static_cast<uint8_t>(*c);
  uint8_t r, g, b;
  hsv2rgb(static_cast<float>(h % 360) / 360.0f, 0.65f, 0.85f, r, g, b);
  return lv_color_make(r, g, b);
}

// --- widgets -----------------------------------------------------------------

lv_obj_t* listScr;                 // nullptr while freed (lazy build)
lv_obj_t* detailScr;
lv_obj_t* col;                     // card column (child of listScr)
lv_obj_t* wifiLabel;
lv_obj_t* totalVal;
lv_obj_t* totalSol;
lv_obj_t* card[kDcaMaxPlans];
lv_obj_t* cardLogo[kDcaMaxPlans];    // lv_image (logo) — hidden for monograms
lv_obj_t* cardChip[kDcaMaxPlans];    // monogram fallback circle label
lv_obj_t* cardTicker[kDcaMaxPlans];
lv_obj_t* cardValue[kDcaMaxPlans];
lv_obj_t* cardCountdown[kDcaMaxPlans];
lv_obj_t* cardSub[kDcaMaxPlans];
lv_obj_t* emptyLabel;
lv_obj_t* emptyHint;

lv_obj_t* dLogo;
lv_obj_t* dChip;
lv_obj_t* dTicker;
lv_obj_t* dPrice;
lv_obj_t* dStatVal[4];               // BUY / BUYS / HELD / VALUE
lv_obj_t* nbCap;
lv_obj_t* dCountdown;
lv_obj_t* pauseBtn;
lv_obj_t* pauseLabel;
lv_obj_t* detailHint;

lv_obj_t* newCard;                   // trailing "+ New plan" card

int8_t detailSlot = -1;

void openDetail(uint8_t slot);   // defined below
lv_obj_t* buildDetailScreen();
lv_obj_t* buildListScreen();

// Group membership must track visibility exactly: hidden cards in the
// focus group swallow LV_KEY_NEXT presses (focus moves to an invisible
// card and the buttons look dead). Rebuilds the group with the visible
// cards only — the plan slots plus the always-visible "+ New plan" card.
void groupSetCards(bool keepFocus) {
  if (!listScr) return;
  lv_obj_t* visible[kDcaMaxPlans + 1];
  uint8_t n = 0;
  for (size_t i = 0; i < g_ui.planCount; i++) visible[n++] = card[i];
  visible[n++] = newCard;
  uiGroupSet(visible, n, keepFocus);
}

// Monogram-chip fallback: white initials on the token color (48 px circle).
void fillChip(lv_obj_t* chip, const char* ticker) {
  size_t tl = strlen(ticker);
  char init[4] = { 0, 0, 0, 0 };
  if (tl <= 3) strlcpy(init, ticker, sizeof(init));
  else { init[0] = ticker[0]; init[1] = ticker[1]; }
  lv_label_set_text(chip, init);
  lv_obj_set_style_bg_color(chip, tokenColor(ticker), 0);
}

// Position value text, honoring the SOL/USD unit toggle ("--" unknown).
void fmtValue(const DcaPlan& p, char* buf, size_t n) {
  double usd = uiPlanValueUsd(p);
  if (usd <= 0.0) { strlcpy(buf, "--", n); return; }
  if (g_ui.amountInSol && g_ui.solUsd > 0.0f) {
    char q[12];
    uiFmtQty(usd / static_cast<double>(g_ui.solUsd), q, sizeof(q));
    snprintf(buf, n, "%s SOL", q);
  } else {
    uiFmtUsd(usd, buf, n);
  }
}

// --- header: total portfolio value + SOL equivalent ----------------------------

void headerFill() {
  if (!listScr) return;
  double total = 0.0;
  bool anyPrice = false;
  for (size_t i = 0; i < g_ui.planCount; i++) {
    double v = uiPlanValueUsd(g_ui.plans[i]);
    if (v > 0.0) { total += v; anyPrice = true; }
  }
  char buf[20];
  if (anyPrice) {
    uiFmtUsd(total, buf, sizeof(buf));
    lv_obj_set_style_text_color(totalVal, UI_COL_TEXT, 0);
  } else {
    strlcpy(buf, g_ui.planCount > 0 ? "$--" : "$0.00", sizeof(buf));
    lv_obj_set_style_text_color(totalVal, UI_COL_MUTED, 0);
  }
  lv_label_set_text(totalVal, buf);

  if (anyPrice && g_ui.solUsd > 0.0f) {
    snprintf(buf, sizeof(buf), "~%.2f SOL", total / static_cast<double>(g_ui.solUsd));
    lv_label_set_text(totalSol, buf);
    lv_obj_set_hidden(totalSol, false);
  } else {
    lv_obj_set_hidden(totalSol, true);
  }
}

// Fill one card from its plan slot.
void cardFill(uint8_t slot) {
  const DcaPlan& p = g_ui.plans[slot];
  bool paused = uiPlanPaused(p);
  bool od = !paused && uiPlanOverdue(slot);

  const lv_image_dsc_t* logo = logoFor(p.ticker);
  if (logo) {
    lv_image_set_src(cardLogo[slot], logo);
    lv_obj_set_hidden(cardLogo[slot], false);
    lv_obj_set_hidden(cardChip[slot], true);
  } else {
    fillChip(cardChip[slot], p.ticker);
    lv_obj_set_hidden(cardChip[slot], false);
    lv_obj_set_hidden(cardLogo[slot], true);
  }

  lv_label_set_text(cardTicker[slot], p.ticker);
  lv_obj_set_style_text_color(cardTicker[slot], paused ? UI_COL_MUTED : UI_COL_TEXT, 0);

  char buf[24];
  fmtValue(p, buf, sizeof(buf));
  lv_label_set_text(cardValue[slot], buf);
  lv_obj_set_style_text_color(cardValue[slot], paused ? UI_COL_MUTED : UI_COL_TEXT, 0);

  char cd[12];
  if (paused) strlcpy(cd, "paused", sizeof(cd));
  else uiFmtCountdown(p.nextBuyEpoch, g_ui.epoch, cd, sizeof(cd));
  lv_label_set_text(cardCountdown[slot], cd);
  lv_obj_set_style_text_color(cardCountdown[slot],
      paused ? UI_COL_MUTED : (od ? UI_COL_WARNING : UI_COL_MUTED), 0);

  char amt[16];
  uiFmtAmount(p, amt, sizeof(amt));
  char qty[12];
  uiFmtQty(static_cast<double>(p.holdingsHeld), qty, sizeof(qty));
  snprintf(buf, sizeof(buf), "%s held " LV_SYMBOL_BULLET " %s", qty, amt);
  lv_label_set_text(cardSub[slot], buf);

  lv_obj_set_style_border_color(card[slot], od ? UI_COL_WARNING : UI_COL_BORDER, 0);
}

void cardFocused(lv_event_t* e) {
  lv_obj_t* target = lv_event_get_target_obj(e);
  lv_obj_scroll_to_view(target, LV_ANIM_ON);
  uiThemeFocusPop(target);
}

void cardClicked(lv_event_t* e) {
  lv_obj_t* target = lv_event_get_target_obj(e);
  uiThemePress(target);
  uint8_t slot = static_cast<uint8_t>(
      reinterpret_cast<intptr_t>(lv_obj_get_user_data(target)));
  openDetail(slot);
}

void newCardClicked(lv_event_t* e) {
  uiThemePress(lv_event_get_target_obj(e));
  uiScreenCreateShow();
}

// --- detail view ---------------------------------------------------------------

void detailFill() {
  if (detailSlot < 0 || detailSlot >= static_cast<int8_t>(g_ui.planCount)) return;
  const DcaPlan& p = g_ui.plans[detailSlot];
  bool paused = uiPlanPaused(p);
  bool od = !paused && uiPlanOverdue(static_cast<uint8_t>(detailSlot));

  const lv_image_dsc_t* logo = logoFor(p.ticker);
  if (logo) {
    lv_image_set_src(dLogo, logo);
    lv_obj_set_hidden(dLogo, false);
    lv_obj_set_hidden(dChip, true);
  } else {
    fillChip(dChip, p.ticker);
    lv_obj_set_hidden(dChip, false);
    lv_obj_set_hidden(dLogo, true);
  }

  lv_label_set_text(dTicker, p.ticker);

  if (p.priceUsd > 0.0f) {
    char pr[16];
    snprintf(pr, sizeof(pr), "$%.2f", static_cast<double>(p.priceUsd));
    lv_label_set_text(dPrice, pr);
    lv_obj_set_style_text_color(dPrice, UI_COL_TEXT, 0);
  } else {
    lv_label_set_text(dPrice, "$--");
    lv_obj_set_style_text_color(dPrice, UI_COL_MUTED, 0);
  }

  char buf[20];
  uiFmtAmount(p, buf, sizeof(buf));
  lv_label_set_text(dStatVal[0], buf);
  snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(p.buys));
  lv_label_set_text(dStatVal[1], buf);
  uiFmtQty(static_cast<double>(p.holdingsHeld), buf, sizeof(buf));
  lv_label_set_text(dStatVal[2], buf);
  fmtValue(p, buf, sizeof(buf));
  lv_label_set_text(dStatVal[3], buf);

  lv_label_set_text(pauseLabel, paused ? LV_SYMBOL_PLAY "  Resume" : LV_SYMBOL_PAUSE "  Pause");
  lv_label_set_text(detailHint, paused ? "push resume  " LV_SYMBOL_BULLET "  back"
                                      : "push pause  " LV_SYMBOL_BULLET "  back");
  if (paused) {
    lv_label_set_text(nbCap, "STATUS");
    lv_label_set_text(dCountdown, "PAUSED");
    lv_obj_set_style_text_color(dCountdown, UI_COL_WARNING, 0);
    return;
  }
  lv_label_set_text(nbCap, "NEXT BUY");

  char cd[12];
  uiFmtCountdown(p.nextBuyEpoch, g_ui.epoch, cd, sizeof(cd));
  lv_label_set_text(dCountdown, cd);
  lv_obj_set_style_text_color(dCountdown, od ? UI_COL_WARNING : UI_COL_TEXT, 0);
}

void openDetail(uint8_t slot) {
  if (!detailScr) buildDetailScreen();
  detailSlot = static_cast<int8_t>(slot);
  detailFill();
  lv_obj_t* one[1] = { pauseBtn };
  uiGroupSet(one, 1, false);
  lv_screen_load_anim(detailScr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 160, 0, false);
}

lv_obj_t* buildDetailScreen() {
  detailScr = lv_obj_create(nullptr);
  uiThemeScreen(detailScr);

  // Circular panel: on the round display a rounded rectangle's corners were
  // cut off by the bezel; a ring reads as deliberate. Every text row below
  // is placed inside the circle's chord at its height.
  lv_obj_t* detailPanel = lv_obj_create(detailScr);
  lv_obj_set_size(detailPanel, 232, 232);
  lv_obj_center(detailPanel);
  uiThemeCard(detailPanel);
  lv_obj_set_style_radius(detailPanel, LV_RADIUS_CIRCLE, 0);

  dLogo = lv_image_create(detailScr);
  lv_obj_align(dLogo, LV_ALIGN_TOP_LEFT, 38, 44);

  dChip = uiThemeLabel(detailScr, &lv_font_montserrat_20, lv_color_white());
  lv_obj_set_size(dChip, 48, 48);
  lv_obj_align(dChip, LV_ALIGN_TOP_LEFT, 38, 44);
  lv_obj_set_style_bg_opa(dChip, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(dChip, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_text_align(dChip, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_pad_top(dChip, 12, 0);

  dTicker = uiThemeLabel(detailScr, &lv_font_montserrat_20, UI_COL_TEXT);
  uiThemeTextBounds(dTicker, 110, 24);
  lv_obj_align(dTicker, LV_ALIGN_TOP_LEFT, 96, 44);

  // 20 pt (not 28): "$1234.56" must fit the 110 px zone without ellipsis.
  dPrice = uiThemeLabel(detailScr, &lv_font_montserrat_20, UI_COL_TEXT);
  uiThemeTextBounds(dPrice, 110, 24);
  lv_obj_align(dPrice, LV_ALIGN_TOP_LEFT, 96, 68);

  static const char* STAT_CAPTIONS[4] = { "BUY", "BUYS", "HELD", "VALUE" };
  const int statX[4] = { 26, 124, 26, 124 };
  const int statY[4] = { 100, 100, 130, 130 };
  for (int i = 0; i < 4; i++) {
    lv_obj_t* cap = uiThemeLabel(detailScr, &lv_font_montserrat_12, UI_COL_MUTED);
    lv_label_set_text(cap, STAT_CAPTIONS[i]);
    lv_obj_align(cap, LV_ALIGN_TOP_LEFT, statX[i], statY[i]);
    dStatVal[i] = uiThemeLabel(detailScr, &lv_font_montserrat_14, UI_COL_TEXT);
    uiThemeTextBounds(dStatVal[i], 94, 18);
    lv_obj_align(dStatVal[i], LV_ALIGN_TOP_LEFT, statX[i], statY[i] + 15);
  }

  nbCap = uiThemeLabel(detailScr, &lv_font_montserrat_12, UI_COL_MUTED);
  lv_label_set_text(nbCap, "NEXT BUY");
  lv_obj_align(nbCap, LV_ALIGN_TOP_LEFT, 40, 168);

  dCountdown = uiThemeLabel(detailScr, &lv_font_montserrat_14, UI_COL_TEXT);
  uiThemeTextBounds(dCountdown, 96, 18);
  lv_obj_set_style_text_align(dCountdown, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_align(dCountdown, LV_ALIGN_TOP_RIGHT, -40, 160);

  pauseBtn = lv_obj_create(detailScr);
  lv_obj_set_size(pauseBtn, 140, 28);
  lv_obj_align(pauseBtn, LV_ALIGN_BOTTOM_MID, 0, -32);
  uiThemeButton(pauseBtn);
  lv_obj_set_style_border_width(pauseBtn, 3, LV_STATE_FOCUSED);
  lv_obj_set_style_border_color(pauseBtn, UI_COL_TEXT, LV_STATE_FOCUSED);
  lv_obj_set_clickable(pauseBtn, true);
  pauseLabel = uiThemeLabel(pauseBtn, &lv_font_montserrat_14, UI_COL_TEXT);
  lv_label_set_text(pauseLabel, "Pause plan");
  lv_obj_center(pauseLabel);

  detailHint = uiThemeLabel(detailScr, &lv_font_montserrat_12, UI_COL_PRIMARY);
  uiThemeTextBounds(detailHint, 130, 16);
  lv_obj_set_style_text_align(detailHint, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(detailHint, "push pause  " LV_SYMBOL_BULLET "  back");
  lv_obj_align(detailHint, LV_ALIGN_BOTTOM_MID, 0, -16);

  return detailScr;
}

// --- list screen builder (lazy — see the heap note at the top) -------------------

lv_obj_t* buildListScreen() {
  listScr = lv_obj_create(nullptr);
  uiThemeScreen(listScr);

  lv_obj_t* title = uiThemeLabel(listScr, &lv_font_montserrat_12, UI_COL_EYEBROW);
  lv_obj_set_style_text_letter_space(title, 1, 0);
  lv_label_set_text(title, "PORTFOLIO");
  lv_obj_align(title, LV_ALIGN_TOP_MID, -8, 14);

  // Wi-Fi glyph right of the title (the round panel clips the top-right
  // corner, so it rides the title row; x <= 172 stays inside the chord).
  wifiLabel = uiThemeLabel(listScr, &lv_font_montserrat_12, UI_COL_MUTED);
  lv_label_set_text(wifiLabel, LV_SYMBOL_WIFI);
  lv_obj_align(wifiLabel, LV_ALIGN_TOP_MID, 44, 14);

  totalVal = uiThemeLabel(listScr, &lv_font_montserrat_28, UI_COL_TEXT);
  lv_label_set_text(totalVal, "$0.00");
  lv_obj_align(totalVal, LV_ALIGN_TOP_MID, 0, 26);

  totalSol = uiThemeLabel(listScr, &lv_font_montserrat_12, UI_COL_MUTED);
  lv_obj_align(totalSol, LV_ALIGN_TOP_MID, 0, 56);
  lv_obj_set_hidden(totalSol, true);

  // The column and cards stay inside the circle's ~180 px bottom chord.
  col = lv_obj_create(listScr);
  lv_obj_set_size(col, 190, 110);
  lv_obj_align(col, LV_ALIGN_TOP_MID, 0, 76);
  lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(col, 0, 0);
  lv_obj_set_style_pad_all(col, 1, 0);
  lv_obj_set_style_pad_row(col, 4, 0);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_scroll_dir(col, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(col, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_set_style_bg_color(col, UI_COL_PRIMARY, LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_opa(col, LV_OPA_COVER, LV_PART_SCROLLBAR);
  lv_obj_set_style_width(col, 3, LV_PART_SCROLLBAR);
  lv_obj_set_style_radius(col, 2, LV_PART_SCROLLBAR);

  for (size_t i = 0; i < kDcaMaxPlans; i++) {
    card[i] = lv_obj_create(col);
    lv_obj_set_size(card[i], 182, 56);
    uiThemeCard(card[i]);
    lv_obj_set_clickable(card[i], true);
    lv_obj_set_user_data(card[i], reinterpret_cast<void*>(static_cast<intptr_t>(i)));
    lv_obj_add_event_cb(card[i], cardClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(card[i], cardFocused, LV_EVENT_FOCUSED, nullptr);
    // NOT added to the focus group here — groupSetCards() tracks visibility.

    // Card zones are bounded for the 182 px card and its countdown column.
    cardLogo[i] = lv_image_create(card[i]);
    lv_obj_align(cardLogo[i], LV_ALIGN_LEFT_MID, 0, 0);
    lv_image_set_scale(cardLogo[i], 204);   // 48 -> 38 px inside the compact card

    cardChip[i] = uiThemeLabel(card[i], &lv_font_montserrat_14, lv_color_white());
    lv_obj_set_size(cardChip[i], 38, 38);
    lv_obj_align(cardChip[i], LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_set_style_bg_opa(cardChip[i], LV_OPA_COVER, 0);
    lv_obj_set_style_radius(cardChip[i], LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_text_align(cardChip[i], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(cardChip[i], 10, 0);

    cardTicker[i] = uiThemeLabel(card[i], &lv_font_montserrat_14, UI_COL_TEXT);
    uiThemeTextBounds(cardTicker[i], 60, 18);
    lv_obj_align(cardTicker[i], LV_ALIGN_TOP_LEFT, 52, 4);

    cardValue[i] = uiThemeLabel(card[i], &lv_font_montserrat_16, UI_COL_TEXT);
    uiThemeTextBounds(cardValue[i], 122, 18);
    lv_obj_align(cardValue[i], LV_ALIGN_TOP_LEFT, 52, 21);

    cardSub[i] = uiThemeLabel(card[i], &lv_font_montserrat_12, UI_COL_MUTED);
    uiThemeTextBounds(cardSub[i], 122, 15);
    lv_obj_align(cardSub[i], LV_ALIGN_TOP_LEFT, 52, 39);

    cardCountdown[i] = uiThemeLabel(card[i], &lv_font_montserrat_12, UI_COL_MUTED);
    uiThemeTextBounds(cardCountdown[i], 60, 15);
    lv_obj_set_style_text_align(cardCountdown[i], LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(cardCountdown[i], LV_ALIGN_TOP_RIGHT, -8, 6);

    lv_obj_set_hidden(card[i], true);
  }

  // Trailing "+ New plan" card: always visible, sits after the last plan.
  newCard = lv_obj_create(col);
  lv_obj_set_size(newCard, 182, 44);
  uiThemeCard(newCard);
  lv_obj_set_clickable(newCard, true);
  lv_obj_add_event_cb(newCard, newCardClicked, LV_EVENT_CLICKED, nullptr);
  lv_obj_add_event_cb(newCard, cardFocused, LV_EVENT_FOCUSED, nullptr);
  lv_obj_t* plus = uiThemeLabel(newCard, &lv_font_montserrat_14, UI_COL_PRIMARY);
  lv_label_set_text(plus, LV_SYMBOL_PLUS "  New plan");
  lv_obj_center(plus);

  emptyLabel = uiThemeLabel(listScr, &lv_font_montserrat_14, UI_COL_PRIMARY);
  lv_label_set_text(emptyLabel, LV_SYMBOL_LIST "  no plans yet");
  lv_obj_align(emptyLabel, LV_ALIGN_CENTER, 0, 24);
  emptyHint = uiThemeLabel(listScr, &lv_font_montserrat_12, UI_COL_MUTED);
  uiThemeTextBounds(emptyHint, 180, 16);
  lv_obj_set_style_text_align(emptyHint, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(emptyHint, "start with " LV_SYMBOL_PLUS " New plan");
  lv_obj_align(emptyHint, LV_ALIGN_CENTER, 0, 44);

  uiThemeControlHint(listScr, "open");

  // Cards/empty state reflect the plan mirror immediately on build.
  for (size_t i = 0; i < kDcaMaxPlans; i++) {
    if (i < g_ui.planCount) {
      cardFill(static_cast<uint8_t>(i));
      lv_obj_set_hidden(card[i], false);
    } else {
      lv_obj_set_hidden(card[i], true);
    }
  }
  bool empty = g_ui.planCount == 0;
  lv_obj_set_hidden(emptyLabel, !empty);
  lv_obj_set_hidden(emptyHint, !empty);

  return listScr;
}

} // namespace

// Eager half of the bring-up (ui::begin): the logo descriptors are static
// and cheap; the list screen itself builds lazily on first show.
lv_obj_t* uiScreenDcaCreate() {
  buildLogos();
  return nullptr;
}

bool uiScreenDcaOnList() {
  return listScr && lv_screen_active() == listScr;
}

// Free the card column but keep the screen shell (header stays up during
// the 160 ms exit animation): lets the create wheel build its drum before
// the shell is auto-deleted — the two full screens never coexist.
void uiScreenDcaShrinkList() {
  if (!listScr || !col) return;
  for (size_t i = 0; i < kDcaMaxPlans; i++) lv_anim_delete(card[i], nullptr);
  lv_anim_delete(newCard, nullptr);
  lv_obj_delete(col);   // cards + "+ New plan" go with it
  col = nullptr;
  uiGroupSet(nullptr, 0, false);   // no dangling focused objects
}

void uiScreenDcaListFreed() {
  // The screen object was deleted via an exit transition's auto_del; only
  // the pointers need clearing (card/* pointers dangle with it — every
  // entry point guards on listScr).
  listScr = nullptr;
  g_ui.dcaScreen = nullptr;
}

void uiScreenDcaReleaseDetail() {
  if (!detailScr || lv_screen_active() == detailScr) return;
  lv_obj_delete(detailScr);
  detailScr = nullptr;
}

void uiScreenDcaShow() {
  // Returning from the create wheel? Free its drum FIRST so this list can
  // rebuild underneath (return-trip heap peak — see uiScreenCreateShrink).
  bool fromCreate = g_ui.createScreen && lv_screen_active() == g_ui.createScreen;
  if (fromCreate) uiScreenCreateShrink();
  if (!listScr) { buildListScreen(); g_ui.dcaScreen = listScr; }
  detailSlot = -1;
  headerFill();
  groupSetCards(false);
  // Lazy screens (menu / create) free their widgets on exit via auto_del.
  bool freeMenu = g_ui.menuScreen && lv_screen_active() == g_ui.menuScreen;
  lv_screen_load_anim(listScr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 160, 0,
                      fromCreate || freeMenu);
  if (fromCreate) uiScreenCreateFreed();
  if (freeMenu) uiScreenMenuFreed();
}

void uiScreenPetShow() {
  if (detailSlot >= 0) detailSlot = -1;
  uiGroupSet(nullptr, 0, false);   // the pet screen has no focusables
  // Lazy screens (menu / create / portfolio list / lock / ota / connect)
  // free on exit.
  bool freeMenu = g_ui.menuScreen && lv_screen_active() == g_ui.menuScreen;
  bool freeCreate = g_ui.createScreen && lv_screen_active() == g_ui.createScreen;
  bool freeDca = listScr && lv_screen_active() == listScr;
  bool freeLock = g_ui.lockScreen && lv_screen_active() == g_ui.lockScreen;
  bool freeOta = g_ui.otaScreen && lv_screen_active() == g_ui.otaScreen;
  bool freeConnect = g_ui.connectScreen && lv_screen_active() == g_ui.connectScreen;
  bool freeNearby = g_ui.nearbyScreen && lv_screen_active() == g_ui.nearbyScreen;
  lv_screen_load_anim(g_ui.petScreen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 160, 0,
                      freeMenu || freeCreate || freeDca || freeLock ||
                      freeOta || freeConnect || freeNearby);
  if (freeMenu) uiScreenMenuFreed();
  if (freeCreate) uiScreenCreateFreed();
  if (freeDca) uiScreenDcaListFreed();
  if (freeLock) uiScreenLockFreed();
  if (freeOta) uiScreenOtaFreed();
  if (freeConnect) uiScreenConnectFreed();
  if (freeNearby) uiScreenNearbyFreed();
}

void uiScreenDcaPlansChanged() {
  if (listScr) {
    for (size_t i = 0; i < kDcaMaxPlans; i++) {
      if (i < g_ui.planCount) {
        cardFill(static_cast<uint8_t>(i));
        lv_obj_set_hidden(card[i], false);
      } else {
        lv_obj_set_hidden(card[i], true);
      }
    }
    bool empty = g_ui.planCount == 0;
    lv_obj_set_hidden(emptyLabel, !empty);
    lv_obj_set_hidden(emptyHint, !empty);
    // Keep group membership in sync with visibility, but only while a
    // portfolio screen is up — the menu screen owns the group when active.
    lv_obj_t* act = lv_screen_active();
    if (act == listScr || act == detailScr) groupSetCards(true);
    headerFill();
  }
  if (detailSlot >= 0) {
    if (detailSlot < static_cast<int8_t>(g_ui.planCount)) detailFill();
    else uiScreenDcaCloseDetail();
  }
  uiScreenPetPlansChanged();
}

void uiScreenDcaHeaderChanged() {
  headerFill();   // guards on listScr itself
  if (g_ui.amountInSol) uiScreenDcaTick();   // SOL-denominated labels moved
}

void uiScreenDcaTick() {
  if (!listScr) return;
  for (size_t i = 0; i < g_ui.planCount; i++) cardFill(static_cast<uint8_t>(i));
  if (detailSlot >= 0) detailFill();
}

bool uiScreenDcaDetailOpen() {
  return detailSlot >= 0 && lv_screen_active() == detailScr;
}

void uiScreenDcaCloseDetail() {
  detailSlot = -1;
  groupSetCards(true);
  lv_screen_load_anim(listScr, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 160, 0, false);
}

void uiScreenDcaToggleAmountUnit() {
  if (g_ui.solUsd <= 0.0f) return;   // SOL mode needs the rate
  g_ui.amountInSol = !g_ui.amountInSol;
  uiScreenDcaTick();
  uiScreenPetPlansChanged();   // the chip honors the unit too
}

bool uiScreenDcaFocusAdvance() {
  if (!listScr) return false;
  lv_obj_t* f = lv_group_get_focused(g_ui.group);
  lv_obj_t* first = g_ui.planCount > 0 ? card[0] : newCard;
  if (!f || f == newCard) lv_group_focus_obj(first);
  else lv_group_focus_next(g_ui.group);
  return true;
}

bool uiScreenDcaFocusPrev() {
  if (!listScr) return false;
  lv_obj_t* f = lv_group_get_focused(g_ui.group);
  lv_obj_t* first = g_ui.planCount > 0 ? card[0] : newCard;
  if (!f || f == first) lv_group_focus_obj(newCard);
  else lv_group_focus_prev(g_ui.group);
  return true;
}

void uiScreenDcaTogglePause() {
  if (detailSlot < 0 || detailSlot >= static_cast<int8_t>(g_ui.planCount)) return;
  if (g_ui.actions.togglePause)
    g_ui.actions.togglePause(static_cast<uint8_t>(detailSlot));
}

void uiScreenDcaSetWifiOnline(bool online) {
  if (!listScr) return;
  lv_obj_set_style_text_color(wifiLabel, online ? UI_COL_PRIMARY : UI_COL_MUTED, 0);
}
