/*
  theme.h — Finagotchi app design tokens applied to the LVGL UI layer.

  Palette and metrics mirror the mobile app (finagotchi_mobile_app/DESIGN.md).
  Montserrat stands in for DM Sans (closest LVGL built-in). The display is a
  240x240 ROUND panel: the corners are not visible, so critical content must
  stay inside the inscribed circle (UI_SAFE margin).
*/

#pragma once

#include <lvgl.h>

// --- landing-blue color tokens ---
#define UI_COL_BG             lv_color_hex(0x0B2858)
#define UI_COL_DEEP_BLUE      lv_color_hex(0x123D77)
#define UI_COL_BLUE           lv_color_hex(0x2668B8)
#define UI_COL_LIGHT_BLUE     lv_color_hex(0x91BEE9)
#define UI_COL_FROST          lv_color_hex(0xC2D9EF)
#define UI_COL_SURFACE        lv_color_hex(0x143766)
#define UI_COL_DIALOG_DEEP    lv_color_hex(0x1D5592)
#define UI_COL_BORDER         lv_color_hex(0x9DCBFF)
#define UI_COL_PRIMARY        lv_color_hex(0x8DC9F6)
#define UI_COL_PRIMARY_PRESSED lv_color_hex(0xB6DCF9)
#define UI_COL_ON_PRIMARY     lv_color_hex(0x102E55)
#define UI_COL_REWARD         lv_color_hex(0xF8B43C)
#define UI_COL_TEXT           lv_color_hex(0xF6FAFF)
#define UI_COL_MUTED          lv_color_hex(0xE0EBF8)
#define UI_COL_EYEBROW        lv_color_hex(0xB2D8F8)
#define UI_COL_INK            lv_color_hex(0x173D6A)
#define UI_COL_INK_MUTED      lv_color_hex(0x4D6C91)
#define UI_COL_DANGER         lv_color_hex(0xF36F7C)
#define UI_COL_WARNING        lv_color_hex(0xE9B846)
#define UI_COL_SUCCESS        lv_color_hex(0x7ED6A7)

// --- radius tokens ---
constexpr int32_t UI_RADIUS_CARD  = 16;
constexpr int32_t UI_RADIUS_PILL  = 999;
constexpr int32_t UI_RADIUS_SMALL = 10;

// --- spacing scale (4 / 8 / 16 / 24) ---
constexpr int32_t UI_SP1 = 4;
constexpr int32_t UI_SP2 = 8;
constexpr int32_t UI_SP3 = 16;
constexpr int32_t UI_SP4 = 24;

// Safe margin inside the round display's inscribed circle.
constexpr int32_t UI_SAFE = 20;

// Re-init the default theme with the app palette (call after uiDisplayInit).
void uiThemeInit();

// Five-stop landing gradient, scrolling off (chrome is hand-placed).
void uiThemeScreen(lv_obj_t* scr);

// Transparent landing glass; active remains readable over the frost stop.
void uiThemeGlass(lv_obj_t* obj, bool active = true);

// Glass card with a light-blue focus ring.
void uiThemeCard(lv_obj_t* obj);

// Opaque navy -> deep-blue dialog surface.
void uiThemeDialog(lv_obj_t* obj);

lv_obj_t* uiThemeLabel(lv_obj_t* parent, const lv_font_t* font, lv_color_t color);
