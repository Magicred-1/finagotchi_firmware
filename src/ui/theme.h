/*
  theme.h — Finagotchi app design tokens applied to the LVGL UI layer.

  The watch-inspired palette pairs black/graphite surfaces with vivid accents.
  Montserrat provides bold, glanceable type with the LVGL built-in fonts. The
  display is a 240x240 ROUND panel: the corners are not visible, so critical
  content must stay inside the inscribed circle (UI_SAFE margin).
*/

#pragma once

#include <lvgl.h>

// --- watch-style black, graphite and vivid system accents ---
#define UI_COL_BG             lv_color_hex(0x000000)
#define UI_COL_DEEP_BLUE      lv_color_hex(0x0A3A6E)
#define UI_COL_BLUE           lv_color_hex(0x0A84FF)
#define UI_COL_LIGHT_BLUE     lv_color_hex(0x64D2FF)
#define UI_COL_FROST          lv_color_hex(0xC7E9FF)
#define UI_COL_SURFACE        lv_color_hex(0x1C1C1E)
#define UI_COL_DIALOG_DEEP    lv_color_hex(0x2C2C2E)
#define UI_COL_BORDER         lv_color_hex(0x48484A)
#define UI_COL_PRIMARY        lv_color_hex(0x0A84FF)
#define UI_COL_PRIMARY_PRESSED lv_color_hex(0x409CFF)
#define UI_COL_ON_PRIMARY     lv_color_hex(0xFFFFFF)
#define UI_COL_REWARD         lv_color_hex(0xFFD60A)
#define UI_COL_TEXT           lv_color_hex(0xF5F5F7)
#define UI_COL_MUTED          lv_color_hex(0x98989F)
#define UI_COL_EYEBROW        lv_color_hex(0x64D2FF)
#define UI_COL_INK            lv_color_hex(0xF5F5F7)
#define UI_COL_INK_MUTED      lv_color_hex(0x98989F)
#define UI_COL_DANGER         lv_color_hex(0xFF453A)
#define UI_COL_WARNING        lv_color_hex(0xFFD60A)
#define UI_COL_SUCCESS        lv_color_hex(0x30D158)

// --- radius tokens ---
constexpr int32_t UI_RADIUS_CARD  = 18;
constexpr int32_t UI_RADIUS_PILL  = 999;
constexpr int32_t UI_RADIUS_SMALL = 10;

// --- spacing scale (4 / 8 / 16 / 24) ---
constexpr int32_t UI_SP1 = 4;
constexpr int32_t UI_SP2 = 8;
constexpr int32_t UI_SP3 = 16;
constexpr int32_t UI_SP4 = 24;

// Safe margin inside the round display's inscribed circle.
constexpr int32_t UI_SAFE = 20;

// Control-hint band: centered near y216, where the circle is ~140 px wide.
constexpr int32_t UI_HINT_BOTTOM = 16;
constexpr int32_t UI_HINT_W = 140;

// Re-init the default theme with the app palette (call after uiDisplayInit).
void uiThemeInit();

// Solid black screen, scrolling off (chrome is hand-placed).
void uiThemeScreen(lv_obj_t* scr);

// Borderless graphite control surface.
void uiThemeGlass(lv_obj_t* obj, bool active = true);

// Graphite card with a blue focus ring.
void uiThemeCard(lv_obj_t* obj);

// Opaque graphite dialog surface.
void uiThemeDialog(lv_obj_t* obj);

lv_obj_t* uiThemeLabel(lv_obj_t* parent, const lv_font_t* font, lv_color_t color);

// Constrain dynamic text to its own layout region; wrap or ellipsize overflow.
void uiThemeTextBounds(lv_obj_t* label, int32_t w, int32_t h, bool wrap = false);

// Short board-specific controls, inside the round panel's bottom safe band.
lv_obj_t* uiThemeControlHint(lv_obj_t* parent, const char* action);

// Rounded actionable surface and short transform-only interaction feedback.
void uiThemeButton(lv_obj_t* obj, bool primary = false);
void uiThemePress(lv_obj_t* obj);
void uiThemeFocusPop(lv_obj_t* obj);
void uiThemeNudge(lv_obj_t* obj, int32_t distance);
