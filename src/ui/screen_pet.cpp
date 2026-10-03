/*
  screen_pet.cpp — LVGL pet screen: a full-screen canvas bound to the pet
  engine's TFT_eSprite framebuffer (the sprite holds ONLY the pet scene now),
  with the chrome rebuilt as LVGL widgets layered above:
  - stage badge (top-left) and battery gauge (top-right)
  - sync spinner + caption while advertising (replaces drawSyncWait)
  - next-buy chip: auto-rotating plan line with cross-fade, amber border
    when overdue (replaces drawDcaLine)
  - stats bar: streak / points / happiness (replaces drawStatsBar)
  - gain toasts on lv_layer_top (replaces drawToasts)
*/

#include "ui_internal.h"

namespace {

lv_obj_t* canvas;
lv_obj_t* badgeLabel;
lv_obj_t* battLabel;
lv_obj_t* battBar;
lv_obj_t* spinner;
lv_obj_t* spinnerCaption;
lv_obj_t* chip;
lv_obj_t* chipLabel;
lv_obj_t* statVal[3];
lv_timer_t* chipRotateTimer;

uint8_t chipSlot = 0;

lv_obj_t* makeLabel(lv_obj_t* parent, const lv_font_t* font, lv_color_t color) {
  lv_obj_t* l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, color, 0);
  return l;
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
  lv_obj_set_style_text_color(chipLabel, p.enabled ? (od ? UI_AMBER : UI_TEXT) : UI_DIM, 0);
  lv_obj_set_style_border_color(chip, od ? UI_AMBER : UI_LINE, 0);
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
  lv_obj_set_style_bg_color(scr, UI_NAVY, 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_set_scrollable(scr, false);

  // Pet scene canvas: straight view onto the sprite framebuffer (both are
  // RGB565 in SPI wire order -> LV_COLOR_FORMAT_RGB565_SWAPPED).
  canvas = lv_canvas_create(scr);
  lv_canvas_set_buffer(canvas, pet->frameBuffer(), pet->frameWidth(),
                       pet->frameHeight(), LV_COLOR_FORMAT_RGB565_SWAPPED);
  lv_obj_center(canvas);

  // Stage badge (top-left).
  badgeLabel = makeLabel(scr, &lv_font_montserrat_12, UI_TEXT);
  lv_obj_set_style_bg_color(badgeLabel, UI_BADGE, 0);
  lv_obj_set_style_bg_opa(badgeLabel, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(badgeLabel, 4, 0);
  lv_obj_set_style_pad_hor(badgeLabel, 6, 0);
  lv_obj_set_style_pad_ver(badgeLabel, 3, 0);
  lv_obj_align(badgeLabel, LV_ALIGN_TOP_LEFT, 10, 6);

  // Battery gauge (top-right): % label + level bar, hidden on USB power.
  battLabel = makeLabel(scr, &lv_font_montserrat_12, UI_TEXT);
  lv_obj_align(battLabel, LV_ALIGN_TOP_RIGHT, -38, 8);
  battBar = lv_bar_create(scr);
  lv_obj_set_size(battBar, 26, 9);
  lv_obj_align(battBar, LV_ALIGN_TOP_RIGHT, -8, 10);
  lv_bar_set_range(battBar, 0, 100);
  lv_obj_set_style_bg_color(battBar, UI_LINE, 0);
  lv_obj_set_style_bg_color(battBar, UI_MINT, LV_PART_INDICATOR);

  // Sync spinner + caption (advertising scene).
  spinner = lv_spinner_create(scr);
  lv_spinner_set_anim_params(spinner, 1000, 270);
  lv_obj_set_size(spinner, 26, 26);
  lv_obj_align(spinner, LV_ALIGN_TOP_MID, 0, 26);
  lv_obj_set_style_arc_color(spinner, UI_CYAN, LV_PART_INDICATOR);
  spinnerCaption = makeLabel(scr, &lv_font_montserrat_12, UI_DIM);
  lv_label_set_text(spinnerCaption, "waiting for connection");
  lv_obj_align(spinnerCaption, LV_ALIGN_TOP_MID, 0, 58);
  lv_obj_set_hidden(spinner, true);
  lv_obj_set_hidden(spinnerCaption, true);

  // Next-buy chip (above the stats bar).
  chip = lv_obj_create(scr);
  lv_obj_set_size(chip, 196, 30);
  lv_obj_align(chip, LV_ALIGN_BOTTOM_MID, 0, -46);
  lv_obj_set_style_bg_color(chip, UI_BADGE, 0);
  lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(chip, 8, 0);
  lv_obj_set_style_border_width(chip, 1, 0);
  lv_obj_set_style_border_color(chip, UI_LINE, 0);
  lv_obj_set_style_pad_all(chip, 0, 0);
  lv_obj_set_scrollable(chip, false);
  chipLabel = makeLabel(chip, &lv_font_montserrat_14, UI_TEXT);
  lv_obj_center(chipLabel);
  lv_obj_set_hidden(chip, true);

  // Stats bar: separator + 3 columns (value over caption).
  lv_obj_t* sep = lv_obj_create(scr);
  lv_obj_set_size(sep, 220, 1);
  lv_obj_align(sep, LV_ALIGN_BOTTOM_MID, 0, -36);
  lv_obj_set_style_bg_color(sep, UI_LINE, 0);
  lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(sep, 0, 0);
  lv_obj_set_scrollable(sep, false);

  static const char* CAPTIONS[3] = { "streak", "points", "happy" };
  const int cols[3] = { -80, 0, 80 };
  for (int i = 0; i < 3; i++) {
    statVal[i] = makeLabel(scr, &lv_font_montserrat_16, UI_TEXT);
    lv_label_set_text(statVal[i], "0");
    lv_obj_align(statVal[i], LV_ALIGN_BOTTOM_MID, cols[i], -18);
    lv_obj_t* cap = makeLabel(scr, &lv_font_montserrat_12, UI_DIM);
    lv_label_set_text(cap, CAPTIONS[i]);
    lv_obj_align(cap, LV_ALIGN_BOTTOM_MID, cols[i], -4);
  }

  chipRotateTimer = lv_timer_create(chipRotate, 4000, nullptr);
  return scr;
}

void uiScreenPetFrame(float nowSec) {
  g_ui.pet->render(nowSec);
  lv_obj_invalidate(canvas);
}

void uiScreenPetSyncWait(bool on) {
  if (on) {
    lv_obj_set_hidden(spinner, false);
    lv_obj_set_hidden(spinnerCaption, false);
  } else {
    lv_obj_set_hidden(spinner, true);
    lv_obj_set_hidden(spinnerCaption, true);
  }
}

void uiScreenPetSetStats(uint32_t streakDays, uint32_t points, uint8_t happiness) {
  char buf[12];
  uiFmtVal(streakDays, buf, sizeof(buf));
  lv_label_set_text(statVal[0], buf);
  uiFmtVal(points, buf, sizeof(buf));
  lv_label_set_text(statVal[1], buf);
  snprintf(buf, sizeof(buf), "%u", happiness > 100 ? 100 : happiness);
  lv_label_set_text(statVal[2], buf);
}

void uiScreenPetSetSubStage(uint8_t subStage) {
  const char* name = uiStageName(subStage);
  lv_label_set_text(badgeLabel, name);
  if (name[0]) lv_obj_set_hidden(badgeLabel, false);
  else lv_obj_set_hidden(badgeLabel, true);
}

void uiScreenPetSetBattery(int pct) {
  if (pct < 0) {
    lv_obj_set_hidden(battLabel, true);
    lv_obj_set_hidden(battBar, true);
    return;
  }
  pct = pct > 100 ? 100 : pct;
  char buf[5];
  snprintf(buf, sizeof(buf), "%d", pct);
  lv_label_set_text(battLabel, buf);
  lv_bar_set_value(battBar, pct, LV_ANIM_OFF);
  // Green > 60%, amber 25-60%, red below (same thresholds as the old gauge).
  lv_color_t c = pct > 60 ? UI_MINT : (pct > 25 ? UI_AMBER : UI_PINK);
  lv_obj_set_style_bg_color(battBar, c, LV_PART_INDICATOR);
  lv_obj_set_hidden(battLabel, false);
  lv_obj_set_hidden(battBar, false);
}

void uiScreenPetToast(const char* text) {
  lv_obj_t* t = makeLabel(lv_layer_top(), &lv_font_montserrat_16, UI_MINT);
  lv_label_set_text(t, text);
  lv_obj_set_style_bg_color(t, UI_NAVY, 0);
  lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(t, 6, 0);
  lv_obj_set_style_pad_hor(t, 8, 0);
  lv_obj_set_style_pad_ver(t, 4, 0);
  lv_obj_align(t, LV_ALIGN_CENTER, 0, 48);

  // Float up ~40 px over 1.2 s (ease out), hold, fade 0.7 s, then delete —
  // same timing as the old sprite toasts, now with real alpha.
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, t);
  lv_anim_set_values(&a, 48, 8);
  lv_anim_set_duration(&a, 1200);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&a, reinterpret_cast<lv_anim_exec_xcb_t>(lv_obj_set_y));
  lv_anim_start(&a);

  lv_anim_t f;
  lv_anim_init(&f);
  lv_anim_set_var(&f, t);
  lv_anim_set_values(&f, LV_OPA_COVER, LV_OPA_TRANSP);
  lv_anim_set_duration(&f, 700);
  lv_anim_set_delay(&f, 1800);
  lv_anim_set_exec_cb(&f, [](void* obj, int32_t v) {
    lv_obj_set_style_opa(static_cast<lv_obj_t*>(obj), static_cast<lv_opa_t>(v), 0);
  });
  lv_anim_set_completed_cb(&f, toastDone);
  lv_anim_start(&f);
}

void uiScreenPetPlansChanged() {
  if (chipSlot >= g_ui.planCount) chipSlot = 0;
  chipFill();
  lv_timer_reset(chipRotateTimer);
}

void uiScreenPetChipAdvance() {
  if (g_ui.planCount == 0) return;
  uint8_t from = chipSlot;
  for (size_t k = 1; k <= g_ui.planCount; k++) {
    uint8_t cand = static_cast<uint8_t>((from + k) % g_ui.planCount);
    if (g_ui.plans[cand].enabled) {
      chipShow(cand, true);
      lv_timer_reset(chipRotateTimer);
      return;
    }
  }
}

void uiScreenPetTick() {
  chipFill();   // countdown text (1 s cadence; cheap when unchanged)
}
