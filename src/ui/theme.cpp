/*
  theme.cpp — see theme.h. One shared implementation of the app's look so
  the pet screen, portfolio screen, overlays and toasts stay consistent.
*/

#include "theme.h"
#include <cstdio>

void uiThemeInit() {
  lv_display_t* disp = lv_display_get_default();
  lv_theme_t* th = lv_theme_default_init(disp, UI_COL_PRIMARY, UI_COL_REWARD,
                                         true /* dark */, LV_FONT_DEFAULT);
  lv_display_set_theme(disp, th);
}

void uiThemeScreen(lv_obj_t* scr) {
  lv_obj_set_style_bg_color(scr, UI_COL_BG, 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_set_scrollable(scr, false);
}

void uiThemeGlass(lv_obj_t* obj, bool active) {
  lv_obj_set_style_bg_color(obj, UI_COL_SURFACE, 0);
  lv_obj_set_style_bg_opa(obj, active ? LV_OPA_60 : LV_OPA_30, 0);
  lv_obj_set_style_border_width(obj, 1, 0);
  lv_obj_set_style_border_color(obj, UI_COL_BORDER, 0);
  lv_obj_set_style_border_opa(obj, active ? LV_OPA_60 : LV_OPA_30, 0);
  lv_obj_set_style_pad_all(obj, 0, 0);
  lv_obj_set_scrollable(obj, false);
}

void uiThemeCard(lv_obj_t* obj) {
  uiThemeGlass(obj, true);
  lv_obj_set_style_radius(obj, UI_RADIUS_CARD, 0);
  lv_obj_set_style_bg_color(obj, UI_COL_SURFACE, LV_STATE_FOCUSED);
  lv_obj_set_style_bg_opa(obj, LV_OPA_60, LV_STATE_FOCUSED);
  lv_obj_set_style_bg_color(obj, UI_COL_SURFACE, LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(obj, LV_OPA_60, LV_STATE_PRESSED);
  lv_obj_set_style_border_color(obj, UI_COL_PRIMARY, LV_STATE_FOCUSED);
  lv_obj_set_style_border_opa(obj, LV_OPA_COVER, LV_STATE_FOCUSED);
  lv_obj_set_style_border_width(obj, 2, LV_STATE_FOCUSED);
}

void uiThemeDialog(lv_obj_t* obj) {
  lv_obj_set_style_bg_color(obj, UI_COL_BG, 0);
  lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(obj, UI_RADIUS_CARD, 0);
  lv_obj_set_style_border_width(obj, 1, 0);
  lv_obj_set_style_border_color(obj, UI_COL_BORDER, 0);
  lv_obj_set_style_border_opa(obj, LV_OPA_30, 0);
  lv_obj_set_scrollable(obj, false);
}

lv_obj_t* uiThemeLabel(lv_obj_t* parent, const lv_font_t* font, lv_color_t color) {
  lv_obj_t* l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, color, 0);
  return l;
}

void uiThemeTextBounds(lv_obj_t* label, int32_t w, int32_t h, bool wrap) {
  lv_obj_set_size(label, w, h);
  lv_label_set_long_mode(label, wrap ? LV_LABEL_LONG_WRAP : LV_LABEL_LONG_DOT);
}

// The hint sits in the round panel's bottom band, where the visible chord
// is only ~150 px wide: keep the text short (rotation/L = next is implied
// by the scrolling list, so only confirm + back are spelled out).
lv_obj_t* uiThemeControlHint(lv_obj_t* parent, const char* action) {
  char text[40];
#ifdef FINAGOTCHI_PCB_V1
  snprintf(text, sizeof(text), "push %s " LV_SYMBOL_BULLET " hold back", action);
#else
  snprintf(text, sizeof(text), "hold %s " LV_SYMBOL_BULLET " R back", action);
#endif
  lv_obj_t* hint = uiThemeLabel(parent, &lv_font_montserrat_12, UI_COL_INK);
  uiThemeTextBounds(hint, UI_HINT_W, 16);
  lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(hint, text);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -UI_HINT_BOTTOM);
  return hint;
}

void uiThemeButton(lv_obj_t* obj, bool primary) {
  uiThemeGlass(obj, true);
  lv_obj_set_style_radius(obj, UI_RADIUS_PILL, 0);
  lv_obj_set_style_bg_color(obj, primary ? UI_COL_PRIMARY : UI_COL_BLUE, 0);
  lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(obj, primary ? UI_COL_PRIMARY_PRESSED : UI_COL_SURFACE,
                            LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_STATE_PRESSED);
  lv_obj_set_style_border_width(obj, 2, 0);
  lv_obj_set_style_border_color(obj, primary ? UI_COL_PRIMARY : UI_COL_BORDER, 0);
  lv_obj_set_style_border_opa(obj, LV_OPA_COVER, 0);
}

// Interaction feedback is deliberately LAYER-FREE: transform_scale (and
// opacity) make LVGL render the object through an ARGB layer buffer the
// size of the object (~33 KB for a menu row), which this heap never has —
// the allocation fails and the object blinks out mid-animation. Translate
// and outline styles draw directly into the frame buffer.

namespace {

void animTranslateY(void* o, int32_t v) {
  lv_obj_set_style_translate_y(static_cast<lv_obj_t*>(o), v, 0);
}

void animOutline(void* o, int32_t v) {
  lv_obj_set_style_outline_width(static_cast<lv_obj_t*>(o), v, 0);
}

// One out-and-back pulse; always ends at 0, so an interrupted pulse can
// never leave the object displaced (translate is relative to the layout,
// unlike lv_obj_set_y on an aligned object).
void pulse(lv_obj_t* obj, lv_anim_exec_xcb_t cb, int32_t peak,
           uint32_t outMs, uint32_t backMs) {
  lv_anim_delete(obj, cb);
  cb(obj, 0);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, obj);
  lv_anim_set_values(&a, 0, peak);
  lv_anim_set_duration(&a, outMs);
  lv_anim_set_playback_duration(&a, backMs);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&a, cb);
  lv_anim_start(&a);
}

} // namespace

void uiThemePress(lv_obj_t* obj) {
  if (!obj) return;
  pulse(obj, animTranslateY, 3, 70, 110);   // a small physical "push"
}

void uiThemeFocusPop(lv_obj_t* obj) {
  if (!obj) return;
  lv_obj_set_style_outline_color(obj, UI_COL_PRIMARY, 0);
  lv_obj_set_style_outline_opa(obj, LV_OPA_COVER, 0);
  lv_obj_set_style_outline_pad(obj, 2, 0);
  pulse(obj, animOutline, 3, 90, 140);       // soft ring flash
}

void uiThemeNudge(lv_obj_t* obj, int32_t distance) {
  if (!obj) return;
  pulse(obj, animTranslateY, distance, 80, 100);
}
