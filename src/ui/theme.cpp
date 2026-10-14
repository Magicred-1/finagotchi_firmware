/*
  theme.cpp — see theme.h. One shared implementation of the app's look so
  the pet screen, portfolio screen, overlays and toasts stay consistent.
*/

#include "theme.h"

namespace {

lv_grad_dsc_t landingGradient;
lv_grad_dsc_t dialogGradient;

void initGradients() {
  const lv_color_t landing[] = {
    UI_COL_BG, UI_COL_DEEP_BLUE, UI_COL_BLUE, UI_COL_LIGHT_BLUE, UI_COL_FROST
  };
  const uint8_t stops[] = { 0, 77, 140, 212, 255 };  // 0 / 30 / 55 / 83 / 100%
  lv_grad_init_stops(&landingGradient, landing, nullptr, stops, 5);
  lv_grad_vertical_init(&landingGradient);

  const lv_color_t dialog[] = { UI_COL_BG, UI_COL_DEEP_BLUE, UI_COL_DIALOG_DEEP };
  const uint8_t dialogStops[] = { 0, 166, 255 };     // 0 / 65 / 100%
  lv_grad_init_stops(&dialogGradient, dialog, nullptr, dialogStops, 3);
  lv_grad_vertical_init(&dialogGradient);
}

} // namespace

void uiThemeInit() {
  initGradients();
  lv_display_t* disp = lv_display_get_default();
  lv_theme_t* th = lv_theme_default_init(disp, UI_COL_PRIMARY, UI_COL_REWARD,
                                         true /* dark */, LV_FONT_DEFAULT);
  lv_display_set_theme(disp, th);
}

void uiThemeScreen(lv_obj_t* scr) {
  lv_obj_set_style_bg_color(scr, UI_COL_BG, 0);
  lv_obj_set_style_bg_grad(scr, &landingGradient, 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_set_scrollable(scr, false);
}

void uiThemeGlass(lv_obj_t* obj, bool active) {
  lv_obj_set_style_bg_color(obj, UI_COL_SURFACE, 0);
  lv_obj_set_style_bg_opa(obj, active ? LV_OPA_40 : LV_OPA_20, 0);
  lv_obj_set_style_border_width(obj, 1, 0);
  lv_obj_set_style_border_color(obj, UI_COL_BORDER, 0);
  lv_obj_set_style_border_opa(obj, active ? LV_OPA_30 : LV_OPA_20, 0);
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
  lv_obj_set_style_bg_grad(obj, &dialogGradient, 0);
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
