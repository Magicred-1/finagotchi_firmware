/*
  screen_dca.cpp — LVGL DCA positions screen (replaces drawDcaPage):
  - scrollable column of up to kDcaMaxPlans plan cards; each card shows the
    token logo (lv_image from the PROGMEM RGB565 arrays in token_logos.h,
    with a procedural monogram-chip fallback for unknown tickers), ticker,
    buy amount (USD, or SOL after the BTN1-double unit toggle), next-buy
    countdown and buys/held
  - overdue plans get an amber border
  - focus group driven by the keypad indev: BTN2 short focuses the next
    card (framework focus visuals), BTN1 short opens the detail view
  - detail view: big price, stats grid, countdown + progress bar
  - empty state with a hint when no plans are mirrored
*/

#include "ui_internal.h"
#include "../token_logos.h"

namespace {

// --- token logos (PROGMEM RGB565 arrays, SPI wire order -> SWAPPED cf) ------

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
    d.header.cf = LV_COLOR_FORMAT_RGB565_SWAPPED;
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
lv_obj_t* card[kDcaMaxPlans];
lv_obj_t* cardLogo[kDcaMaxPlans];    // lv_image (logo) — hidden for monograms
lv_obj_t* cardChip[kDcaMaxPlans];    // monogram fallback circle label
lv_obj_t* cardTicker[kDcaMaxPlans];
lv_obj_t* cardAmount[kDcaMaxPlans];
lv_obj_t* cardCountdown[kDcaMaxPlans];
lv_obj_t* cardSub[kDcaMaxPlans];
lv_obj_t* emptyLabel;
lv_obj_t* emptyHint;

lv_obj_t* dLogo;
lv_obj_t* dChip;
lv_obj_t* dTicker;
lv_obj_t* dPrice;
lv_obj_t* dStatVal[4];               // BUY / BUYS / HELD / VALUE
lv_obj_t* dCountdown;
lv_obj_t* dBar;

int8_t detailSlot = -1;

void openDetail(uint8_t slot);   // defined below

lv_obj_t* makeLabel(lv_obj_t* parent, const lv_font_t* font, lv_color_t color) {
  lv_obj_t* l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, color, 0);
  return l;
}

void styleScreen(lv_obj_t* scr) {
  lv_obj_set_style_bg_color(scr, UI_NAVY, 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_set_scrollable(scr, false);
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

// Fill one card from its plan slot.
void cardFill(uint8_t slot) {
  const DcaPlan& p = g_ui.plans[slot];
  bool od = uiPlanOverdue(slot);

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
  lv_obj_set_style_text_color(cardTicker[slot], p.enabled ? UI_TEXT : UI_DIM, 0);

  char buf[24];
  uiFmtAmount(p, buf, sizeof(buf));
  lv_label_set_text(cardAmount[slot], buf);

  char cd[12];
  uiFmtCountdown(p.nextBuyEpoch, g_ui.epoch, cd, sizeof(cd));
  lv_label_set_text(cardCountdown[slot], cd);
  lv_obj_set_style_text_color(cardCountdown[slot], od ? UI_AMBER : UI_TEXT, 0);

  snprintf(buf, sizeof(buf), "%lu buys  %lu held",
           static_cast<unsigned long>(p.buys),
           static_cast<unsigned long>(p.holdingsHeld));
  lv_label_set_text(cardSub[slot], buf);

  lv_obj_set_style_border_color(card[slot], od ? UI_AMBER : UI_LINE, 0);
}

void cardClicked(lv_event_t* e) {
  uint8_t slot = static_cast<uint8_t>(
      reinterpret_cast<intptr_t>(lv_obj_get_user_data(lv_event_get_target_obj(e))));
  openDetail(slot);
}

// --- detail view ---------------------------------------------------------------

void detailFill() {
  if (detailSlot < 0 || detailSlot >= static_cast<int8_t>(g_ui.planCount)) return;
  const DcaPlan& p = g_ui.plans[detailSlot];
  bool od = uiPlanOverdue(static_cast<uint8_t>(detailSlot));

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
    lv_obj_set_style_text_color(dPrice, UI_MINT, 0);
  } else {
    lv_label_set_text(dPrice, "$--");
    lv_obj_set_style_text_color(dPrice, UI_DIM, 0);
  }

  char buf[20];
  uiFmtAmount(p, buf, sizeof(buf));
  lv_label_set_text(dStatVal[0], buf);
  snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(p.buys));
  lv_label_set_text(dStatVal[1], buf);
  snprintf(buf, sizeof(buf), "%lu", static_cast<unsigned long>(p.holdingsHeld));
  lv_label_set_text(dStatVal[2], buf);
  if (p.priceUsd > 0.0f) {
    char val[12];
    uiFmtVal(static_cast<uint32_t>(p.holdingsHeld * p.priceUsd), val, sizeof(val));
    snprintf(buf, sizeof(buf), "~$%s", val);
  } else {
    strlcpy(buf, "--", sizeof(buf));
  }
  lv_label_set_text(dStatVal[3], buf);

  char cd[12];
  uiFmtCountdown(p.nextBuyEpoch, g_ui.epoch, cd, sizeof(cd));
  lv_label_set_text(dCountdown, cd);
  lv_obj_set_style_text_color(dCountdown, od ? UI_AMBER : UI_TEXT, 0);

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
  lv_obj_set_style_bg_color(dBar, od ? UI_AMBER : UI_MINT, LV_PART_INDICATOR);
}

void openDetail(uint8_t slot) {
  detailSlot = static_cast<int8_t>(slot);
  detailFill();
  lv_screen_load_anim(detailScr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
}

} // namespace

lv_obj_t* uiScreenDcaCreate() {
  buildLogos();

  // --- list screen ---
  listScr = lv_obj_create(nullptr);
  styleScreen(listScr);

  lv_obj_t* title = makeLabel(listScr, &lv_font_montserrat_12, UI_DIM);
  lv_label_set_text(title, "DCA POSITIONS");
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 8);

  lv_obj_t* line = lv_obj_create(listScr);
  lv_obj_set_size(line, 216, 1);
  lv_obj_align(line, LV_ALIGN_TOP_MID, 0, 24);
  lv_obj_set_style_bg_color(line, UI_LINE, 0);
  lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(line, 0, 0);
  lv_obj_set_scrollable(line, false);

  // Scrollable card column.
  lv_obj_t* col = lv_obj_create(listScr);
  lv_obj_set_size(col, 240, 186);
  lv_obj_align(col, LV_ALIGN_TOP_MID, 0, 28);
  lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(col, 0, 0);
  lv_obj_set_style_pad_all(col, 2, 0);
  lv_obj_set_style_pad_row(col, 6, 0);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_scrollbar_mode(col, LV_SCROLLBAR_MODE_OFF);

  for (size_t i = 0; i < kDcaMaxPlans; i++) {
    card[i] = lv_obj_create(col);
    lv_obj_set_size(card[i], 208, 68);
    lv_obj_set_style_bg_color(card[i], UI_BADGE, 0);
    lv_obj_set_style_bg_opa(card[i], LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card[i], 10, 0);
    lv_obj_set_style_border_width(card[i], 1, 0);
    lv_obj_set_style_border_color(card[i], UI_LINE, 0);
    lv_obj_set_style_border_color(card[i], UI_CYAN, LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(card[i], 2, LV_STATE_FOCUSED);
    lv_obj_set_style_pad_all(card[i], 0, 0);
    lv_obj_set_scrollable(card[i], false);
    lv_obj_set_clickable(card[i], true);
    lv_obj_set_user_data(card[i], reinterpret_cast<void*>(static_cast<intptr_t>(i)));
    lv_obj_add_event_cb(card[i], cardClicked, LV_EVENT_CLICKED, nullptr);
    lv_group_add_obj(g_ui.group, card[i]);

    cardLogo[i] = lv_image_create(card[i]);
    lv_obj_align(cardLogo[i], LV_ALIGN_LEFT_MID, 8, 0);

    cardChip[i] = makeLabel(card[i], &lv_font_montserrat_16, lv_color_white());
    lv_obj_set_size(cardChip[i], 48, 48);
    lv_obj_align(cardChip[i], LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_bg_opa(cardChip[i], LV_OPA_COVER, 0);
    lv_obj_set_style_radius(cardChip[i], LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_text_align(cardChip[i], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(cardChip[i], 14, 0);

    cardTicker[i] = makeLabel(card[i], &lv_font_montserrat_16, UI_TEXT);
    lv_obj_align(cardTicker[i], LV_ALIGN_TOP_LEFT, 64, 6);

    cardAmount[i] = makeLabel(card[i], &lv_font_montserrat_14, UI_MINT);
    lv_obj_align(cardAmount[i], LV_ALIGN_TOP_LEFT, 64, 27);

    cardSub[i] = makeLabel(card[i], &lv_font_montserrat_12, UI_DIM);
    lv_obj_align(cardSub[i], LV_ALIGN_TOP_LEFT, 64, 47);

    cardCountdown[i] = makeLabel(card[i], &lv_font_montserrat_14, UI_TEXT);
    lv_obj_align(cardCountdown[i], LV_ALIGN_TOP_RIGHT, -8, 6);

    lv_obj_set_hidden(card[i], true);
  }

  emptyLabel = makeLabel(listScr, &lv_font_montserrat_14, UI_DIM);
  lv_label_set_text(emptyLabel, "no DCA plans yet");
  lv_obj_align(emptyLabel, LV_ALIGN_CENTER, 0, -10);
  emptyHint = makeLabel(listScr, &lv_font_montserrat_12, UI_DIM);
  lv_label_set_text(emptyHint, "open the Finagotchi app");
  lv_obj_align(emptyHint, LV_ALIGN_CENTER, 0, 10);

  lv_obj_t* hint = makeLabel(listScr, &lv_font_montserrat_12, UI_DIM);
  lv_label_set_text(hint, "btn1: open   btn2: next");
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -6);

  // --- detail screen ---
  detailScr = lv_obj_create(nullptr);
  styleScreen(detailScr);

  dLogo = lv_image_create(detailScr);
  lv_obj_align(dLogo, LV_ALIGN_TOP_LEFT, 24, 32);

  dChip = makeLabel(detailScr, &lv_font_montserrat_20, lv_color_white());
  lv_obj_set_size(dChip, 48, 48);
  lv_obj_align(dChip, LV_ALIGN_TOP_LEFT, 24, 32);
  lv_obj_set_style_bg_opa(dChip, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(dChip, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_text_align(dChip, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_pad_top(dChip, 12, 0);

  dTicker = makeLabel(detailScr, &lv_font_montserrat_20, UI_TEXT);
  lv_obj_align(dTicker, LV_ALIGN_TOP_LEFT, 84, 34);

  dPrice = makeLabel(detailScr, &lv_font_montserrat_28, UI_MINT);
  lv_obj_align(dPrice, LV_ALIGN_TOP_LEFT, 84, 58);

  static const char* STAT_CAPTIONS[4] = { "BUY", "BUYS", "HELD", "VALUE" };
  const int statX[4] = { 16, 128, 16, 128 };
  const int statY[4] = { 108, 108, 152, 152 };
  for (int i = 0; i < 4; i++) {
    lv_obj_t* cap = makeLabel(detailScr, &lv_font_montserrat_12, UI_DIM);
    lv_label_set_text(cap, STAT_CAPTIONS[i]);
    lv_obj_align(cap, LV_ALIGN_TOP_LEFT, statX[i], statY[i]);
    dStatVal[i] = makeLabel(detailScr, &lv_font_montserrat_14,
                            (i == 0 || i == 3) ? UI_MINT : UI_TEXT);
    lv_obj_align(dStatVal[i], LV_ALIGN_TOP_LEFT, statX[i], statY[i] + 15);
  }

  lv_obj_t* nbCap = makeLabel(detailScr, &lv_font_montserrat_12, UI_DIM);
  lv_label_set_text(nbCap, "NEXT BUY");
  lv_obj_align(nbCap, LV_ALIGN_TOP_LEFT, 16, 196);

  dCountdown = makeLabel(detailScr, &lv_font_montserrat_16, UI_TEXT);
  lv_obj_align(dCountdown, LV_ALIGN_TOP_RIGHT, -16, 192);

  dBar = lv_bar_create(detailScr);
  lv_obj_set_size(dBar, 208, 6);
  lv_obj_align(dBar, LV_ALIGN_TOP_MID, 0, 216);
  lv_bar_set_range(dBar, 0, 100);
  lv_obj_set_style_bg_color(dBar, UI_LINE, 0);

  lv_obj_t* dHint = makeLabel(detailScr, &lv_font_montserrat_12, UI_DIM);
  lv_label_set_text(dHint, "btn1/btn2: back");
  lv_obj_align(dHint, LV_ALIGN_BOTTOM_MID, 0, -4);

  return listScr;
}

void uiScreenDcaShow() {
  detailSlot = -1;
  lv_screen_load_anim(listScr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
}

void uiScreenPetShow() {
  if (detailSlot >= 0) detailSlot = -1;
  lv_screen_load_anim(g_ui.petScreen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 250, 0, false);
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
  if (empty) {
    lv_obj_set_hidden(emptyLabel, false);
    lv_obj_set_hidden(emptyHint, false);
  } else {
    lv_obj_set_hidden(emptyLabel, true);
    lv_obj_set_hidden(emptyHint, true);
    lv_group_focus_obj(card[0]);
  }
  if (detailSlot >= 0) {
    if (detailSlot < static_cast<int8_t>(g_ui.planCount)) detailFill();
    else uiScreenDcaCloseDetail();
  }
  uiScreenPetPlansChanged();
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
