/*
  theme.cpp — see theme.h. One shared implementation of the app's look so
  the pet screen, portfolio screen, overlays and toasts stay consistent.
*/

#include "theme.h"

void uiThemeInit() {
  lv_display_t* disp = lv_display_get_default();
  lv_theme_t* th = lv_theme_default_init(disp, UI_COL_PRIMARY, UI_COL_PURPLE,
                                         true /* dark */, LV_FONT_DEFAULT);
  lv_display_set_theme(disp, th);
}

void uiThemeScreen(lv_obj_t* scr) {
  lv_obj_set_style_bg_color(scr, UI_COL_BG, 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_set_scrollable(scr, false);
}

void uiThemeCard(lv_obj_t* obj) {
  lv_obj_set_style_bg_color(obj, UI_COL_SURFACE, 0);
  lv_obj_set_style_bg_color(obj, UI_COL_SURFACE_HI, LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(obj, UI_RADIUS_CARD, 0);
  lv_obj_set_style_border_width(obj, 1, 0);
  lv_obj_set_style_border_color(obj, UI_COL_BORDER, 0);
  lv_obj_set_style_border_color(obj, UI_COL_PRIMARY, LV_STATE_FOCUSED);
  lv_obj_set_style_border_width(obj, 2, LV_STATE_FOCUSED);
  lv_obj_set_style_pad_all(obj, 0, 0);
  lv_obj_set_scrollable(obj, false);
}

lv_obj_t* uiThemeLabel(lv_obj_t* parent, const lv_font_t* font, lv_color_t color) {
  lv_obj_t* l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, color, 0);
  return l;
}

lv_obj_t* uiHintBarCreate(lv_obj_t* parent, int32_t yOff) {
  lv_obj_t* bar = uiThemeLabel(parent, &lv_font_montserrat_12, UI_COL_MUTED);
  lv_obj_set_style_text_align(bar, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, yOff);
  return bar;
}

void uiHintBarSet(lv_obj_t* bar, const char* text) {
  lv_label_set_text(bar, text);
}
