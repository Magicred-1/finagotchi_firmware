/*
  theme.h — Finagotchi app design tokens applied to the LVGL UI layer.

  Palette and metrics mirror the mobile app (finagotchi_mobile_app/DESIGN.md).
  Montserrat stands in for Poppins (closest LVGL built-in). The display is a
  240x240 ROUND panel: the corners are not visible, so critical content must
  stay inside the inscribed circle (UI_SAFE margin).
*/

#pragma once

#include <lvgl.h>

// --- color tokens ---
#define UI_COL_BG         lv_color_hex(0x07111F)   // screen background (app navy)
#define UI_COL_SURFACE    lv_color_hex(0x0E1B2E)   // cards / chips
#define UI_COL_SURFACE_HI lv_color_hex(0x162640)   // pressed / hover
#define UI_COL_BORDER     lv_color_hex(0x243651)
#define UI_COL_PRIMARY    lv_color_hex(0x35D7FF)   // cyan: focus, active, CTA
#define UI_COL_PURPLE     lv_color_hex(0x9945FF)   // reward / magic (dca:hit)
#define UI_COL_TEXT       lv_color_hex(0xFFFFFF)
#define UI_COL_MUTED      lv_color_hex(0x8FA2B8)
#define UI_COL_DANGER     lv_color_hex(0xFF647C)
#define UI_COL_WARNING    lv_color_hex(0xFFD166)   // overdue
#define UI_COL_SUCCESS    lv_color_hex(0x5DE2A6)   // gains

// --- radius tokens ---
constexpr int32_t UI_RADIUS_CARD  = 14;
constexpr int32_t UI_RADIUS_PILL  = 999;
constexpr int32_t UI_RADIUS_SMALL = 8;

// --- spacing scale (4 / 8 / 16 / 24) ---
constexpr int32_t UI_SP1 = 4;
constexpr int32_t UI_SP2 = 8;
constexpr int32_t UI_SP3 = 16;
constexpr int32_t UI_SP4 = 24;

// Safe margin inside the round display's inscribed circle.
constexpr int32_t UI_SAFE = 20;

// Re-init the default theme with the app palette (call after uiDisplayInit).
void uiThemeInit();

// Screen background in the app navy, scrolling off (chrome is hand-placed).
void uiThemeScreen(lv_obj_t* scr);

// Card: surface bg, 14 px radius, 1 px border; focused = 2 px cyan ring.
void uiThemeCard(lv_obj_t* obj);

// Caption/value label pair helpers (muted caption over white value).
lv_obj_t* uiThemeLabel(lv_obj_t* parent, const lv_font_t* font, lv_color_t color);

// Bottom hint bar: one tiny centered muted label showing what the buttons
// do on the CURRENT screen ("1: sync   2: portfolio"). Kept short on
// purpose: near the bottom edge of the round panel only a narrow strip of
// the inscribed circle is visible. yOff lets dense screens (pet) float it
// above their bottom rows. Update with uiHintBarSet on state changes.
lv_obj_t* uiHintBarCreate(lv_obj_t* parent, int32_t yOff);
void uiHintBarSet(lv_obj_t* bar, const char* text);
