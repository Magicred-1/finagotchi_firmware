/*
  screen_dca.cpp — LVGL Portfolio/Positions screen (app design language):
  - header: total portfolio value = sum(holdings x price_usd) across plans,
    big USD number with a SOL equivalent under it when the SOL/USD rate is
    known (BLE solusd: / NVS / standalone price feed)
  - scrollable column of up to kDcaMaxPlans position cards: token logo
    (lv_image from the PROGMEM RGB565 arrays in token_logos.h, or a
    monogram-chip fallback), ticker, position value (USD, or SOL after the
    BTN1-double unit toggle), next-buy countdown, holdings/buy line
  - overdue plans get the warning-amber treatment
  - focus group driven by the keypad indev: BTN2 short focuses the next
    card (cyan framework focus ring), BTN1 short opens/closes the detail
  - detail view: price, stats grid, countdown + progress bar
  - empty state with a hint when no plans are mirrored

  No button-hint text anywhere (left = move, right = select — the UI is
  instruction-free).
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
    case 1: rr = x; gg = c; bb = 0; break;
    case 2: rr = 0; gg = c; bb = x; break;
    case 3: rr = 0; gg = x; bb = c; break;
    case 4: rr = x; gg = 0; bb = c; break;
    default: rr = c; gg = 0; bb = x; break;
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
    { "AAPLX", 162, 170, 173 },   // AAPLx  — Apple silver
    { "TSLAX", 232,  33,  39 },   // TSLAx  — Tesla red
    { "NVDAX", 118, 185,   0 },   // NVDAx  — NVIDIA green
    { "METAX",   0, 129, 251 },   // METAx  — Meta blue
    { "MSFTX",   0, 164, 239 },   // MSFTx  — Microsoft blue
    { "AMZNX", 255, 153,   0 },   // AMZNx  — Amazon orange
    { "MSTRX", 247, 147,  26 },   // MSTRx  — Strategy orange
    { "CRCLX",  41,  98, 255 },   // CRCLx  — Circle blue
    { "NFLXX", 229,   9,  20 },   // NFLXx  — Netflix red
    { "COINX",   0,  82, 255 },   // COINx  — Coinbase blue
    { "HOODX",   0, 200,   5 },   // HOODx  — Robinhood green
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

lv_obj_t* listScr;
lv_obj_t* detailScr;
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
lv_obj_t* dBar;

lv_obj_t* newCard;                   // trailing "+ New plan" card

int8_t detailSlot = -1;

void openDetail(uint8_t slot);   // defined below

// Group membership must track visibility exactly: hidden cards in the
// focus group swallow LV_KEY_NEXT presses (focus moves to an invisible
// card and the buttons look dead). Rebuilds the group with the visible
// cards only — the plan slots plus the always-visible "+ New plan" card.
void groupSetCards(bool keepFocus) {
  lv_obj_t* visible[kDcaMaxPlans + 1];
  uint8_t n = 0;
  for (size_t i = 0; i < g_ui.planCount; i++) visible[n++] = card[i];
  visible[n++] = newCard;
  uiGroupSet(visible, n, keepFocus);
}

// Monogram chip fallback: white initials on the token color (48 px circle).
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
  if (g_ui.amountInSol && g_ui.solUsd > 0.0f)
    snprintf(buf, n, "%.4g SOL", usd / static_cast<double>(g_ui.solUsd));
  else
    uiFmtUsd(usd, buf, n);
}

// --- header: total portfolio value + SOL equivalent ----------------------------

void headerFill() {
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
  snprintf(buf, sizeof(buf), "%lu held  %s/buy",
           static_cast<unsigned long>(p.holdingsHeld), amt);
  lv_label_set_text(cardSub[slot], buf);

  lv_obj_set_style_border_color(card[slot], od ? UI_COL_WARNING : UI_COL_BORDER, 0);
}

void cardClicked(lv_event_t* e) {
  uint8_t slot = static_cast<uint8_t>(
      reinterpret_cast<intptr_t>(lv_obj_get_user_data(lv_event_get_target_obj(e))));
  openDetail(slot);
}

void newCardClicked(lv_event_t*) {
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
  snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(p.holdingsHeld));
  lv_label_set_text(dStatVal[2], buf);
  fmtValue(p, buf, sizeof(buf));
  lv_label_set_text(dStatVal[3], buf);

  if (paused) {
    // Big PAUSED instead of countdown + progress (en=0 / epoch=0 plans).
    lv_label_set_text(nbCap, "STATUS");
    lv_label_set_text(dCountdown, "PAUSED");
    lv_obj_set_style_text_color(dCountdown, UI_COL_WARNING, 0);
    lv_obj_set_hidden(dBar, true);
    return;
  }
  lv_label_set_text(nbCap, "NEXT BUY");
  lv_obj_set_hidden(dBar, false);

  char cd[12];
  uiFmtCountdown(p.nextBuyEpoch, g_ui.epoch, cd, sizeof(cd));
  lv_label_set_text(dCountdown, cd);
  lv_obj_set_style_text_color(dCountdown, od ? UI_COL_WARNING : UI_COL_TEXT, 0);

  bool unknown = (g_ui.epoch == 0 || p.nextBuyEpoch == 0);
  int32_t rem = unknown ? 0 : static_cast<int32_t>(p.nextBuyEpoch - g_ui.epoch);
  int32_t pct;
  if (od) pct = 100;
  else if (unknown) pct = 0;
  else {
    float frac = 1.0f - static_cast<float>(rem) / (7.0f * 86400.0f);
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    pct = static_cast<int32_t>(frac * 100.0f);
  }
  lv_bar_set_value(dBar, pct, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(dBar, od ? UI_COL_WARNING : UI_COL_PRIMARY, LV_PART_INDICATOR);
}

void openDetail(uint8_t slot) {
  detailSlot = static_cast<int8_t>(slot);
  detailFill();
  lv_screen_load_anim(detailScr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
}

} // namespace

lv_obj_t* uiScreenDcaCreate() {
  buildLogos();

  // --- portfolio list screen ---
  listScr = lv_obj_create(nullptr);
  uiThemeScreen(listScr);

  lv_obj_t* title = uiThemeLabel(listScr, &lv_font_montserrat_12, UI_COL_MUTED);
  lv_label_set_text(title, "PORTFOLIO");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

  totalVal = uiThemeLabel(listScr, &lv_font_montserrat_28, UI_COL_TEXT);
  lv_label_set_text(totalVal, "$0.00");
  lv_obj_align(totalVal, LV_ALIGN_TOP_MID, 0, 26);

  totalSol = uiThemeLabel(listScr, &lv_font_montserrat_12, UI_COL_MUTED);
  lv_obj_align(totalSol, LV_ALIGN_TOP_MID, 0, 58);
  lv_obj_set_hidden(totalSol, true);

  // Scrollable card column (extends to the bottom safe zone — no hint bar).
  lv_obj_t* col = lv_obj_create(listScr);
  lv_obj_set_size(col, 240, 158);
  lv_obj_align(col, LV_ALIGN_TOP_MID, 0, 76);
  lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(col, 0, 0);
  lv_obj_set_style_pad_all(col, 2, 0);
  lv_obj_set_style_pad_row(col, 6, 0);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_scrollbar_mode(col, LV_SCROLLBAR_MODE_OFF);

  for (size_t i = 0; i < kDcaMaxPlans; i++) {
    card[i] = lv_obj_create(col);
    lv_obj_set_size(card[i], 208, 62);
    uiThemeCard(card[i]);
    lv_obj_set_clickable(card[i], true);
    lv_obj_set_user_data(card[i], reinterpret_cast<void*>(static_cast<intptr_t>(i)));
    lv_obj_add_event_cb(card[i], cardClicked, LV_EVENT_CLICKED, nullptr);
    // NOT added to the focus group here — groupSetCards() tracks visibility.

    cardLogo[i] = lv_image_create(card[i]);
    lv_obj_align(cardLogo[i], LV_ALIGN_LEFT_MID, 7, 0);

    cardChip[i] = uiThemeLabel(card[i], &lv_font_montserrat_16, lv_color_white());
    lv_obj_set_size(cardChip[i], 48, 48);
    lv_obj_align(cardChip[i], LV_ALIGN_LEFT_MID, 7, 0);
    lv_obj_set_style_bg_opa(cardChip[i], LV_OPA_COVER, 0);
    lv_obj_set_style_radius(cardChip[i], LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_text_align(cardChip[i], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(cardChip[i], 14, 0);

    cardTicker[i] = uiThemeLabel(card[i], &lv_font_montserrat_16, UI_COL_TEXT);
    lv_obj_align(cardTicker[i], LV_ALIGN_TOP_LEFT, 62, 7);

    cardValue[i] = uiThemeLabel(card[i], &lv_font_montserrat_16, UI_COL_TEXT);
    lv_obj_align(cardValue[i], LV_ALIGN_TOP_LEFT, 62, 27);

    cardSub[i] = uiThemeLabel(card[i], &lv_font_montserrat_12, UI_COL_MUTED);
    lv_obj_align(cardSub[i], LV_ALIGN_TOP_LEFT, 62, 45);

    cardCountdown[i] = uiThemeLabel(card[i], &lv_font_montserrat_12, UI_COL_MUTED);
    lv_obj_align(cardCountdown[i], LV_ALIGN_TOP_RIGHT, -8, 10);

    lv_obj_set_hidden(card[i], true);
  }

  // Trailing "+ New plan" card: always visible, sits after the last plan.
  newCard = lv_obj_create(col);
  lv_obj_set_size(newCard, 208, 44);
  uiThemeCard(newCard);
  lv_obj_set_clickable(newCard, true);
  lv_obj_add_event_cb(newCard, newCardClicked, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* plus = uiThemeLabel(newCard, &lv_font_montserrat_14, UI_COL_PRIMARY);
  lv_label_set_text(plus, LV_SYMBOL_PLUS "  New plan");
  lv_obj_center(plus);

  emptyLabel = uiThemeLabel(listScr, &lv_font_montserrat_14, UI_COL_MUTED);
  lv_label_set_text(emptyLabel, "no DCA plans yet");
  lv_obj_align(emptyLabel, LV_ALIGN_CENTER, 0, 0);
  emptyHint = uiThemeLabel(listScr, &lv_font_montserrat_12, UI_COL_MUTED);
  lv_label_set_text(emptyHint, "open the app to start a DCA plan");
  lv_obj_align(emptyHint, LV_ALIGN_CENTER, 0, 18);

  // --- detail screen ---
  detailScr = lv_obj_create(nullptr);
  uiThemeScreen(detailScr);

  dLogo = lv_image_create(detailScr);
  lv_obj_align(dLogo, LV_ALIGN_TOP_LEFT, 24, 28);

  dChip = uiThemeLabel(detailScr, &lv_font_montserrat_20, lv_color_white());
  lv_obj_set_size(dChip, 48, 48);
  lv_obj_align(dChip, LV_ALIGN_TOP_LEFT, 24, 28);
  lv_obj_set_style_bg_opa(dChip, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(dChip, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_text_align(dChip, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_pad_top(dChip, 12, 0);

  dTicker = uiThemeLabel(detailScr, &lv_font_montserrat_20, UI_COL_TEXT);
  lv_obj_align(dTicker, LV_ALIGN_TOP_LEFT, 84, 30);

  dPrice = uiThemeLabel(detailScr, &lv_font_montserrat_28, UI_COL_TEXT);
  lv_obj_align(dPrice, LV_ALIGN_TOP_LEFT, 84, 54);

  static const char* STAT_CAPTIONS[4] = { "BUY", "BUYS", "HELD", "VALUE" };
  const int statX[4] = { 24, 128, 24, 128 };
  const int statY[4] = { 104, 104, 148, 148 };
  for (int i = 0; i < 4; i++) {
    lv_obj_t* cap = uiThemeLabel(detailScr, &lv_font_montserrat_12, UI_COL_MUTED);
    lv_label_set_text(cap, STAT_CAPTIONS[i]);
    lv_obj_align(cap, LV_ALIGN_TOP_LEFT, statX[i], statY[i]);
    dStatVal[i] = uiThemeLabel(detailScr, &lv_font_montserrat_14, UI_COL_TEXT);
    lv_obj_align(dStatVal[i], LV_ALIGN_TOP_LEFT, statX[i], statY[i] + 15);
  }

  nbCap = uiThemeLabel(detailScr, &lv_font_montserrat_12, UI_COL_MUTED);
  lv_label_set_text(nbCap, "NEXT BUY");
  lv_obj_align(nbCap, LV_ALIGN_TOP_LEFT, 24, 190);

  dCountdown = uiThemeLabel(detailScr, &lv_font_montserrat_16, UI_COL_TEXT);
  lv_obj_align(dCountdown, LV_ALIGN_TOP_RIGHT, -24, 186);

  dBar = lv_bar_create(detailScr);
  lv_obj_set_size(dBar, 192, 6);
  lv_obj_align(dBar, LV_ALIGN_TOP_MID, 0, 212);
  lv_bar_set_range(dBar, 0, 100);
  lv_obj_set_style_bg_color(dBar, UI_COL_BORDER, 0);

  return listScr;
}

void uiScreenDcaShow() {
  detailSlot = -1;
  headerFill();
  groupSetCards(false);
  // Coming back from the create wheel? Its widgets free on exit (boot-heap
  // discipline): the transition's auto_del deletes the old screen object.
  bool freeCreate = g_ui.createScreen && lv_screen_active() == g_ui.createScreen;
  lv_screen_load_anim(listScr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, freeCreate);
  if (freeCreate) uiScreenCreateFreed();
}

void uiScreenPetShow() {
  if (detailSlot >= 0) detailSlot = -1;
  uiGroupSet(nullptr, 0, false);   // the pet screen has no focusables
  // Lazy screens (menu / create) free their widgets on exit via auto_del.
  bool freeMenu = g_ui.menuScreen && lv_screen_active() == g_ui.menuScreen;
  bool freeCreate = g_ui.createScreen && lv_screen_active() == g_ui.createScreen;
  lv_screen_load_anim(g_ui.petScreen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 250, 0,
                      freeMenu || freeCreate);
  if (freeMenu) uiScreenMenuFreed();
  if (freeCreate) uiScreenCreateFreed();
}

void uiScreenDcaPlansChanged() {
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
  if (detailSlot >= 0) {
    if (detailSlot < static_cast<int8_t>(g_ui.planCount)) detailFill();
    else uiScreenDcaCloseDetail();
  }
  uiScreenPetPlansChanged();
}

void uiScreenDcaHeaderChanged() {
  headerFill();
  if (g_ui.amountInSol) uiScreenDcaTick();   // SOL-denominated labels moved
}

void uiScreenDcaTick() {
  for (size_t i = 0; i < g_ui.planCount; i++) cardFill(static_cast<uint8_t>(i));
  if (detailSlot >= 0) detailFill();
}

bool uiScreenDcaDetailOpen() {
  return detailSlot >= 0 && lv_screen_active() == detailScr;
}

void uiScreenDcaCloseDetail() {
  detailSlot = -1;
  lv_screen_load_anim(listScr, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 250, 0, false);
}

void uiScreenDcaToggleAmountUnit() {
  if (g_ui.solUsd <= 0.0f) return;   // SOL mode needs the rate
  g_ui.amountInSol = !g_ui.amountInSol;
  uiScreenDcaTick();
  uiScreenPetPlansChanged();   // the chip honors the unit too
}

bool uiScreenDcaFocusAdvance() {
  lv_obj_t* f = lv_group_get_focused(g_ui.group);
  if (f && f != newCard) {   // the "+ New plan" card is always last
    lv_group_focus_next(g_ui.group);
    return true;
  }
  if (!f) {   // nothing focused yet (e.g. after a refresh): focus the first
    lv_group_focus_obj(g_ui.planCount > 0 ? card[0] : newCard);
    return true;
  }
  return false;   // on the last card: caller moves to the next screen
}

void uiScreenDcaTogglePause() {
  if (detailSlot < 0 || detailSlot >= static_cast<int8_t>(g_ui.planCount)) return;
  if (g_ui.actions.togglePause)
    g_ui.actions.togglePause(static_cast<uint8_t>(detailSlot));
}
