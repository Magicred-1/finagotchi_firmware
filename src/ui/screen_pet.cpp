/*
  screen_pet.cpp — LVGL pet screen: a full-screen canvas bound to the pet
  engine's TFT_eSprite framebuffer (the sprite holds ONLY the pet scene),
  with the chrome rebuilt as LVGL widgets in the app's design language
  (theme.h):
  - stage badge (top-left) and battery gauge (top-right)
  - sync spinner + caption while advertising
  - next-buy chip: auto-rotating plan pill with cross-fade, warning border
    when overdue
  - stats: streak / points / happiness bar (captions over values)
  - gain/reward toasts on lv_layer_top (success green, dca:hit purple)

  No button-hint text anywhere: the control model is dead simple
  (left = move, right = select) and the UI is instruction-free.
*/

#include "ui_internal.h"

namespace {

lv_obj_t* canvas;
lv_obj_t* badgeLabel;
lv_obj_t* stageTrack;
lv_obj_t* battLabel;
lv_obj_t* spinner;
lv_obj_t* spinnerCaption;
lv_obj_t* chip;
lv_obj_t* chipLabel;
lv_obj_t* statVal[2];
lv_obj_t* happyBar;
lv_timer_t* chipRotateTimer;

uint8_t chipSlot = 0;

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

  // Pet scene canvas: straight view onto the sprite framebuffer (both are
  // big-endian RGB565 -> LV_COLOR_FORMAT_RGB565_SWAPPED).
  canvas = lv_canvas_create(scr);
  lv_canvas_set_buffer(canvas, pet->frameBuffer(), pet->frameWidth(),
                       pet->frameHeight(), LV_COLOR_FORMAT_RGB565_SWAPPED);
  lv_obj_center(canvas);

  // Stage badge (top-left), with the slim stage/XP track under it
  // (PetCanvas stageTrack: white 8% track, primary cyan fill, x/12).
  badgeLabel = uiThemeLabel(scr, &lv_font_montserrat_12, UI_COL_TEXT);
  lv_obj_set_style_bg_color(badgeLabel, UI_COL_SURFACE, 0);
  lv_obj_set_style_bg_opa(badgeLabel, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(badgeLabel, UI_RADIUS_SMALL, 0);
  lv_obj_set_style_pad_hor(badgeLabel, UI_SP2, 0);
  lv_obj_set_style_pad_ver(badgeLabel, UI_SP1, 0);
  lv_obj_align(badgeLabel, LV_ALIGN_TOP_LEFT, 10, 6);

  stageTrack = lv_bar_create(scr);
  lv_obj_set_size(stageTrack, 60, 4);
  lv_obj_align(stageTrack, LV_ALIGN_TOP_LEFT, 12, 28);
  lv_bar_set_range(stageTrack, 0, 12);
  lv_obj_set_style_bg_color(stageTrack, lv_color_hex(0x1B2431), 0);   // white 8% / navy
  lv_obj_set_style_bg_color(stageTrack, UI_COL_PRIMARY, LV_PART_INDICATOR);

  // Battery gauge (top-right): battery symbol + %, colored by level,
  // hidden on USB power.
  battLabel = uiThemeLabel(scr, &lv_font_montserrat_12, UI_COL_MUTED);
  lv_obj_align(battLabel, LV_ALIGN_TOP_RIGHT, -10, 8);

  // Sync spinner + caption (advertising scene).
  spinner = lv_spinner_create(scr);
  lv_spinner_set_anim_params(spinner, 1000, 270);
  lv_obj_set_size(spinner, 26, 26);
  lv_obj_align(spinner, LV_ALIGN_TOP_MID, 0, 26);
  lv_obj_set_style_arc_color(spinner, UI_COL_PRIMARY, LV_PART_INDICATOR);
  spinnerCaption = uiThemeLabel(scr, &lv_font_montserrat_12, UI_COL_MUTED);
  lv_label_set_text(spinnerCaption, "waiting for connection");
  lv_obj_align(spinnerCaption, LV_ALIGN_TOP_MID, 0, 58);
  lv_obj_set_hidden(spinner, true);
  lv_obj_set_hidden(spinnerCaption, true);

  // Next-buy chip (pill, above the stats rows).
  chip = lv_obj_create(scr);
  lv_obj_set_size(chip, 196, 30);
  lv_obj_align(chip, LV_ALIGN_BOTTOM_MID, 0, -56);
  lv_obj_set_style_bg_color(chip, UI_COL_SURFACE, 0);
  lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(chip, UI_RADIUS_PILL, 0);
  lv_obj_set_style_border_width(chip, 1, 0);
  lv_obj_set_style_border_color(chip, UI_COL_BORDER, 0);
  lv_obj_set_style_pad_all(chip, 0, 0);
  lv_obj_set_scrollable(chip, false);
  chipLabel = uiThemeLabel(chip, &lv_font_montserrat_14, UI_COL_TEXT);
  lv_obj_center(chipLabel);
  lv_obj_set_hidden(chip, true);

  // Bottom stats, stacked to stay inside the round panel's inscribed
  // circle: captions (y 200-212), values (y 213-227).
  static const char* CAPTIONS[3] = { "streak", "points", "happy" };
  const int cols[3] = { -44, 0, 44 };
  for (int i = 0; i < 3; i++) {
    lv_obj_t* cap = uiThemeLabel(scr, &lv_font_montserrat_12, UI_COL_MUTED);
    lv_label_set_text(cap, CAPTIONS[i]);
    lv_obj_align(cap, LV_ALIGN_BOTTOM_MID, cols[i], -34);
    if (i < 2) {
      statVal[i] = uiThemeLabel(scr, &lv_font_montserrat_14, UI_COL_TEXT);
      lv_label_set_text(statVal[i], "0");
      lv_obj_align(statVal[i], LV_ALIGN_BOTTOM_MID, cols[i], -19);
    }
  }
  // Happiness shows as a small bar (app's HappinessBar pattern) instead of
  // a raw number; same rose accent the app uses for the heart.
  happyBar = lv_bar_create(scr);
  lv_obj_set_size(happyBar, 38, 6);
  lv_obj_align(happyBar, LV_ALIGN_BOTTOM_MID, cols[2], -23);
  lv_bar_set_range(happyBar, 0, 100);
  lv_obj_set_style_bg_color(happyBar, lv_color_hex(0x1B2431), 0);
  lv_obj_set_style_bg_color(happyBar, UI_COL_DANGER, LV_PART_INDICATOR);

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
  char buf[12];
  uiFmtVal(streakDays, buf, sizeof(buf));
  lv_label_set_text(statVal[0], buf);
  uiFmtVal(points, buf, sizeof(buf));
  lv_label_set_text(statVal[1], buf);
  lv_bar_set_value(happyBar, happiness > 100 ? 100 : happiness, LV_ANIM_OFF);
}

void uiScreenPetSetSubStage(uint8_t subStage) {
  const char* name = uiStageName(subStage);
  lv_label_set_text(badgeLabel, name);
  lv_obj_set_hidden(badgeLabel, !name[0]);
  lv_bar_set_value(stageTrack, subStage < 1 ? 1 : (subStage > 12 ? 12 : subStage),
                   LV_ANIM_ON);
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
  lv_color_t col = reward ? UI_COL_PURPLE : UI_COL_SUCCESS;
  lv_obj_t* t = uiThemeLabel(lv_layer_top(), &lv_font_montserrat_16, col);
  lv_label_set_text(t, text);
  lv_obj_set_style_bg_color(t, UI_COL_SURFACE, 0);
  lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(t, UI_RADIUS_PILL, 0);
  lv_obj_set_style_border_width(t, 1, 0);
  lv_obj_set_style_border_color(t, col, 0);
  lv_obj_set_style_pad_hor(t, 10, 0);
  lv_obj_set_style_pad_ver(t, 5, 0);
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
