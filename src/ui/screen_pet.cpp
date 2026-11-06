/*
  screen_pet.cpp — LVGL pet screen: a full-screen canvas bound to the pet
  engine's TFT_eSprite framebuffer (the sprite holds ONLY the pet scene),
  with the chrome rebuilt as LVGL widgets in the app's design language
  (theme.h):
  - stage badge (top-center) and battery gauge + Wi-Fi glyph (top corners,
    inset past the round panel's clipped corners)
  - sync spinner + caption while advertising
  - next-buy chip: auto-rotating plan pill with cross-fade, warning border
    when overdue
  - stats: streak / points / happiness bar (captions over values)
  - gain/reward toasts on lv_layer_top (success green, dca:hit amber)

  Control hints live on the list screens, leaving the pet canvas uncluttered.
*/

#include "ui_internal.h"

namespace {

lv_obj_t* canvas;
lv_obj_t* badgeLabel;
lv_obj_t* stageTrack;
lv_obj_t* wifiLabel;
lv_obj_t* battLabel;
lv_obj_t* spinner;
lv_obj_t* spinnerCaption;
lv_obj_t* chip;
lv_obj_t* chipLabel;
lv_obj_t* statsLabel;
lv_obj_t* menuHint;
lv_timer_t* chipRotateTimer;

uint8_t chipSlot = 0;
uint8_t petSubStage = 1;
bool petConnected = false;

void badgeFill() {
  const char* name = petConnected ? "Finagotchi" : uiStageName(petSubStage);
  lv_label_set_text(badgeLabel, name);
  lv_obj_set_hidden(badgeLabel, !name[0]);
  lv_bar_set_value(stageTrack,
                   petSubStage < 1 ? 1 : (petSubStage > 12 ? 12 : petSubStage),
                   LV_ANIM_ON);
}

// --- next-buy chip -----------------------------------------------------------

void chipFill() {
  if (g_ui.planCount == 0 || chipSlot >= g_ui.planCount) {
    lv_obj_set_hidden(chip, true);
    return;
  }
  const DcaPlan& p = g_ui.plans[chipSlot];
  char amt[16], cd[12], buf[44];
  uiFmtAmount(p, amt, sizeof(amt));
  uiFmtCountdown(p.nextBuyEpoch, g_ui.epoch, cd, sizeof(cd));
  snprintf(buf, sizeof(buf), "%s  %s  %s", p.ticker, amt, cd);

  bool od = uiPlanOverdue(chipSlot);
  lv_label_set_text(chipLabel, buf);
  lv_obj_set_style_text_color(chipLabel,
      p.enabled ? (od ? UI_COL_WARNING : UI_COL_TEXT) : UI_COL_MUTED, 0);
  lv_obj_set_style_border_color(chip, od ? UI_COL_WARNING : UI_COL_BORDER, 0);
  lv_obj_set_hidden(chip, false);
}

void chipFadeOutDone(lv_anim_t*) {
  chipFill();
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, chipLabel);
  lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
  lv_anim_set_duration(&a, 225);
  lv_anim_set_exec_cb(&a, [](void* obj, int32_t v) {
    lv_obj_set_style_opa(static_cast<lv_obj_t*>(obj), static_cast<lv_opa_t>(v), 0);
  });
  lv_anim_start(&a);
}

// Cross-fade to the given slot (or just refresh the text when the slot stays).
void chipShow(uint8_t slot, bool animate) {
  chipSlot = slot;
  if (!animate) { chipFill(); return; }
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, chipLabel);
  lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
  lv_anim_set_duration(&a, 225);
  lv_anim_set_exec_cb(&a, [](void* obj, int32_t v) {
    lv_obj_set_style_opa(static_cast<lv_obj_t*>(obj), static_cast<lv_opa_t>(v), 0);
  });
  lv_anim_set_completed_cb(&a, chipFadeOutDone);
  lv_anim_start(&a);
}

void chipRotate(lv_timer_t*) {
  if (g_ui.planCount < 2) { chipFill(); return; }
  for (size_t k = 1; k <= g_ui.planCount; k++) {
    uint8_t cand = static_cast<uint8_t>((chipSlot + k) % g_ui.planCount);
    if (g_ui.plans[cand].enabled) { chipShow(cand, true); return; }
  }
}

// --- toasts (lv_layer_top, so they show over either screen) ------------------

void toastDone(lv_anim_t* a) {
  lv_obj_delete_async(static_cast<lv_obj_t*>(a->var));
}

} // namespace

lv_obj_t* uiScreenPetCreate() {
  FinagotchiPet* pet = g_ui.pet;
  lv_obj_t* scr = lv_obj_create(nullptr);
  uiThemeScreen(scr);

  // Borderless black portrait backdrop around the 160px RGB565 canvas.
  // Slightly smaller than the square layout and nudged down: frees a clean
  // chrome band at the top (badge / Wi-Fi / battery) on the round panel.
  lv_obj_t* portrait = lv_obj_create(scr);
  lv_obj_set_size(portrait, 168, 168);
  lv_obj_align(portrait, LV_ALIGN_CENTER, 0, 2);
  lv_obj_set_style_bg_color(portrait, UI_COL_BG, 0);
  lv_obj_set_style_bg_opa(portrait, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(portrait, UI_RADIUS_CARD, 0);
  lv_obj_set_style_border_width(portrait, 0, 0);
  lv_obj_set_style_pad_all(portrait, 0, 0);
  lv_obj_set_scrollable(portrait, false);

  // Pet scene canvas: straight view onto the sprite framebuffer (both are
  // big-endian RGB565 -> LV_COLOR_FORMAT_RGB565_SWAPPED).
  canvas = lv_canvas_create(portrait);
  lv_canvas_set_buffer(canvas, pet->frameBuffer(), pet->frameWidth(),
                       pet->frameHeight(), LV_COLOR_FORMAT_RGB565_SWAPPED);
  lv_obj_center(canvas);

  // Stage badge (top-center, with the slim stage/XP track under it). The
  // round panel clips the corners, so the badge rides the top dead zone
  // instead of the old top-left corner.
  badgeLabel = uiThemeLabel(scr, &lv_font_montserrat_12, UI_COL_TEXT);
  uiThemeGlass(badgeLabel, false);
  lv_obj_set_style_radius(badgeLabel, UI_RADIUS_SMALL, 0);
  lv_obj_set_style_pad_hor(badgeLabel, UI_SP2, 0);
  lv_obj_set_style_pad_ver(badgeLabel, UI_SP1, 0);
  lv_obj_align(badgeLabel, LV_ALIGN_TOP_MID, 0, 8);

  stageTrack = lv_bar_create(scr);
  lv_obj_set_size(stageTrack, 60, 4);
  lv_obj_align(stageTrack, LV_ALIGN_TOP_MID, 0, 30);
  lv_bar_set_range(stageTrack, 0, 12);
  lv_obj_set_style_bg_color(stageTrack, UI_COL_BORDER, 0);
  lv_obj_set_style_bg_opa(stageTrack, LV_OPA_20, 0);
  lv_obj_set_style_bg_color(stageTrack, UI_COL_PRIMARY, LV_PART_INDICATOR);

  // Wi-Fi status glyph (top-left, inset past the circle's edge): accent
  // when online, muted when offline. Always visible — also on USB power.
  wifiLabel = uiThemeLabel(scr, &lv_font_montserrat_12, UI_COL_MUTED);
  lv_label_set_text(wifiLabel, LV_SYMBOL_WIFI);
  lv_obj_align(wifiLabel, LV_ALIGN_TOP_LEFT, 44, 32);

  // Battery gauge (top-right, same inset as the Wi-Fi glyph — the round
  // panel's corner would clip it at the old -10 offset): symbol + %,
  // colored by level, hidden on USB power.
  battLabel = uiThemeLabel(scr, &lv_font_montserrat_12, UI_COL_MUTED);
  lv_obj_align(battLabel, LV_ALIGN_TOP_RIGHT, -44, 32);
  lv_obj_set_hidden(battLabel, true);

  // Sync spinner + caption (advertising scene). Sits below the badge/track
  // row so the two never overlap on the round panel.
  spinner = lv_spinner_create(scr);
  lv_spinner_set_anim_params(spinner, 1000, 270);
  lv_obj_set_size(spinner, 26, 26);
  lv_obj_align(spinner, LV_ALIGN_TOP_MID, 0, 42);
  lv_obj_set_style_arc_color(spinner, UI_COL_PRIMARY, LV_PART_INDICATOR);
  spinnerCaption = uiThemeLabel(scr, &lv_font_montserrat_12, UI_COL_MUTED);
  lv_label_set_text(spinnerCaption, "waiting for connection");
  lv_obj_align(spinnerCaption, LV_ALIGN_TOP_MID, 0, 72);
  lv_obj_set_hidden(spinner, true);
  lv_obj_set_hidden(spinnerCaption, true);

  // Next-buy chip, raised to leave room for one-line stats and the menu hint.
  chip = lv_obj_create(scr);
  lv_obj_set_size(chip, 196, 30);
  lv_obj_align(chip, LV_ALIGN_BOTTOM_MID, 0, -68);
  uiThemeGlass(chip, true);
  lv_obj_set_style_radius(chip, UI_RADIUS_PILL, 0);
  chipLabel = uiThemeLabel(chip, &lv_font_montserrat_14, UI_COL_TEXT);
  uiThemeTextBounds(chipLabel, 182, 20);
  lv_obj_set_style_text_align(chipLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_center(chipLabel);
  lv_obj_set_hidden(chip, true);

  statsLabel = uiThemeLabel(scr, &lv_font_montserrat_12, UI_COL_INK);
  uiThemeTextBounds(statsLabel, 176, 16);
  lv_obj_set_style_text_align(statsLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(statsLabel, "0d  " LV_SYMBOL_BULLET "  0 pts  " LV_SYMBOL_BULLET "  50% :)");
  lv_obj_align(statsLabel, LV_ALIGN_BOTTOM_MID, 0, -32);

  menuHint = uiThemeLabel(scr, &lv_font_montserrat_12, UI_COL_PRIMARY);
  uiThemeTextBounds(menuHint, 140, 16);
  lv_obj_set_style_text_align(menuHint, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(menuHint, LV_SYMBOL_LIST " rotate / push");
  lv_obj_align(menuHint, LV_ALIGN_BOTTOM_MID, 0, -16);

  chipRotateTimer = lv_timer_create(chipRotate, 4000, nullptr);
  return scr;
}

void uiScreenPetFrame(float nowSec) {
  g_ui.pet->render(nowSec);
  lv_obj_invalidate(canvas);
}

void uiScreenPetSyncWait(bool on) {
  lv_obj_set_hidden(spinner, !on);
  lv_obj_set_hidden(spinnerCaption, !on);
}

void uiScreenPetSetStats(uint32_t streakDays, uint32_t points, uint8_t happiness) {
  char streak[12], pts[12], buf[56];
  uiFmtVal(streakDays, streak, sizeof(streak));
  uiFmtVal(points, pts, sizeof(pts));
  snprintf(buf, sizeof(buf), "%sd  " LV_SYMBOL_BULLET "  %s pts  " LV_SYMBOL_BULLET "  %u%% :)",
           streak, pts, static_cast<unsigned>(happiness > 100 ? 100 : happiness));
  if (strcmp(lv_label_get_text(statsLabel), buf) != 0) {
    lv_label_set_text(statsLabel, buf);
    uiThemeNudge(statsLabel, -2);
  }
}

void uiScreenPetSetSubStage(uint8_t subStage) {
  petSubStage = subStage;
  badgeFill();
}

void uiScreenPetSetConnected(bool connected) {
  petConnected = connected;
  badgeFill();
}

void uiScreenPetSetWifiOnline(bool online) {
  lv_obj_set_style_text_color(wifiLabel, online ? UI_COL_PRIMARY : UI_COL_MUTED, 0);
}

void uiScreenPetSetBattery(int pct) {
  if (pct < 0) {
    lv_obj_set_hidden(battLabel, true);
    return;
  }
  pct = pct > 100 ? 100 : pct;
  const char* sym = pct > 75 ? LV_SYMBOL_BATTERY_FULL :
                    pct > 50 ? LV_SYMBOL_BATTERY_3 :
                    pct > 25 ? LV_SYMBOL_BATTERY_2 : LV_SYMBOL_BATTERY_1;
  char buf[12];
  snprintf(buf, sizeof(buf), "%s %d", sym, pct);
  lv_label_set_text(battLabel, buf);
  lv_color_t c = pct > 60 ? UI_COL_SUCCESS : (pct > 25 ? UI_COL_WARNING : UI_COL_DANGER);
  lv_obj_set_style_text_color(battLabel, c, 0);
  lv_obj_set_hidden(battLabel, false);
}

void uiScreenPetToast(const char* text, bool reward) {
  lv_color_t accent = reward ? UI_COL_REWARD : UI_COL_PRIMARY;
  lv_obj_t* t = uiThemeLabel(lv_layer_top(), &lv_font_montserrat_14, UI_COL_TEXT);
  char msg[96];
  snprintf(msg, sizeof(msg), "%s  %s", reward ? LV_SYMBOL_PLUS : LV_SYMBOL_OK, text);
  lv_label_set_text(t, msg);
  uiThemeTextBounds(t, 180, 28);
  lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_size(t, 200, 36);
  lv_obj_set_style_bg_color(t, UI_COL_SURFACE, 0);
  lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(t, UI_RADIUS_PILL, 0);
  lv_obj_set_style_border_width(t, 2, 0);
  lv_obj_set_style_border_color(t, accent, 0);
  lv_obj_set_style_border_opa(t, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(t, 4, 0);
  lv_obj_align(t, LV_ALIGN_CENTER, 0, 48);

  // Float up ~40 px over 1.2 s (ease out), hold, then slide out and
  // delete. POSITION-ONLY on purpose: an opacity fade makes LVGL render
  // the toast through a layer buffer (~29 KB contiguous for 32-bit alpha)
  // which this device never has free — every fading toast silently failed
  // to draw ("Allocating layer buffer failed" in the LVGL log).
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, t);
  lv_anim_set_values(&a, 48, 8);
  lv_anim_set_duration(&a, 1200);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&a, reinterpret_cast<lv_anim_exec_xcb_t>(lv_obj_set_y));
  lv_anim_start(&a);

  lv_anim_t out;
  lv_anim_init(&out);
  lv_anim_set_var(&out, t);
  lv_anim_set_values(&out, 8, -28);
  lv_anim_set_duration(&out, 600);
  lv_anim_set_delay(&out, 1800);
  lv_anim_set_path_cb(&out, lv_anim_path_ease_in);
  lv_anim_set_exec_cb(&out, reinterpret_cast<lv_anim_exec_xcb_t>(lv_obj_set_y));
  lv_anim_set_completed_cb(&out, toastDone);
  lv_anim_start(&out);
}

void uiScreenPetPlansChanged() {
  if (chipSlot >= g_ui.planCount) chipSlot = 0;
  chipFill();
  lv_timer_reset(chipRotateTimer);
}

void uiScreenPetTick() {
  chipFill();   // countdown text (1 s cadence; cheap when unchanged)
}
