#include "ui_internal.h"
#include "../nearby.h"
#include "../nearby_logo.h"

namespace {

constexpr int CENTER_X = 120;
constexpr int CENTER_Y = 118;
constexpr int SWEEP_RADIUS = 54;

lv_obj_t* nearbyScr;
lv_obj_t* statusLabel;
lv_obj_t* selectedLabel;
lv_obj_t* emptyLabel;
lv_obj_t* nearbyLogo;
lv_obj_t* rings[2];
lv_obj_t* sweepArc;
lv_obj_t* sweepLine;
lv_timer_t* sweepTimer;
lv_obj_t* zoomOutBtn;
lv_obj_t* zoomInBtn;
lv_obj_t* allBtn;
lv_obj_t* dots[kNearbyMaxPeers];
lv_obj_t* dotText[kNearbyMaxPeers];
lv_obj_t* selectionMarker;
lv_obj_t* focusItems[kNearbyMaxPeers + 3];
NearbyPeer peers[kNearbyMaxPeers];
float sweepAngle = 0;
uint8_t peerCount = 0;
uint8_t selected = 0;
uint8_t zoomPct = 100;
bool allSelected = false;

lv_color_t stageColor(uint8_t stage) {
  static const uint32_t colors[] = { 0xD4C8B8, 0xF59E0B, 0x3B82F6, 0x6366F1 };
  return lv_color_hex(colors[stage < 4 ? stage : 0]);
}

const char* zoneName(int16_t rssi) {
  int zone = nearbyZone(rssi);
  return zone == 0 ? "near" : (zone == 1 ? "mid" : "far");
}

void selectionFill() {
  if (!selectedLabel) return;
  if (selected < peerCount) {
    const NearbyPeer& p = peers[selected];
    char buf[56];
    snprintf(buf, sizeof(buf), "Finagotchi %03X " LV_SYMBOL_BULLET " %s",
             static_cast<unsigned>(p.id & 0xFFF), zoneName(p.rssi));
    lv_label_set_text(selectedLabel, buf);
    return;
  }
  char buf[40];
  uint8_t action = selected - peerCount;
  if (action == 0) snprintf(buf, sizeof(buf), "Zoom out  " LV_SYMBOL_BULLET "  %u%%", zoomPct);
  else if (action == 1) snprintf(buf, sizeof(buf), "Zoom in  " LV_SYMBOL_BULLET "  %u%%", zoomPct);
  else strlcpy(buf, allSelected ? "All pets selected" : "Select all pets", sizeof(buf));
  lv_label_set_text(selectedLabel, buf);
}

void refreshPeers() {
  if (!nearbyScr) return;
  peerCount = nearbyCopyPeers(peers, kNearbyMaxPeers);
  uint8_t total = peerCount + 3;
  if (selected >= total) selected = total - 1;

  int baseSize = peerCount <= 2 ? 40 : (peerCount <= 4 ? 34 : 28);
  int dotSize = baseSize * zoomPct / 100;
  if (dotSize < 22) dotSize = 22;
  int half = dotSize / 2;
  float maxRadius = 54 - half;

  if (peerCount > 1) {
    float required = (dotSize + 6) / (2.0f * sinf(static_cast<float>(PI) / peerCount));
    if (required > maxRadius) {
      dotSize = static_cast<int>(2.0f * maxRadius * sinf(static_cast<float>(PI) / peerCount) - 6.0f);
      if (dotSize < 22) dotSize = 22;
      half = dotSize / 2;
      maxRadius = 54 - half;
    }
  }

  bool hasNear = false;
  for (uint8_t i = 0; i < peerCount; i++) hasNear |= nearbyZone(peers[i].rssi) == 0;
  int logoScale = 180 * zoomPct / 100;
  if (hasNear && logoScale > 90) {
    float logoRadius = logoScale * 115.0f / 256.0f * 0.35f;
    if (logoRadius + half + 6 > maxRadius) {
      dotSize = dotSize < 28 ? dotSize : 28;
      half = dotSize / 2;
      maxRadius = 54 - half;
      logoScale = 90;
    }
  }
  float logoRadius = logoScale * 115.0f / 256.0f * 0.35f;
  lv_image_set_scale(nearbyLogo, logoScale);

  float nearRadius = 26.0f * zoomPct / 100.0f;
  if (hasNear) nearRadius = fmaxf(nearRadius, logoRadius + half + 6);
  if (peerCount > 1)
    nearRadius = fmaxf(nearRadius,
                       (dotSize + 6) / (2.0f * sinf(static_cast<float>(PI) / peerCount)));
  if (nearRadius > maxRadius) nearRadius = maxRadius;
  float midRadius = fmaxf(38.0f * zoomPct / 100.0f, nearRadius + dotSize + 4);
  float farRadius = fmaxf(50.0f * zoomPct / 100.0f, midRadius + dotSize + 4);
  if (midRadius > maxRadius) midRadius = maxRadius;
  if (farRadius > maxRadius) farRadius = maxRadius;

  lv_obj_set_size(rings[0], static_cast<int>(nearRadius * 2), static_cast<int>(nearRadius * 2));
  lv_obj_set_size(rings[1], static_cast<int>(farRadius * 2), static_cast<int>(farRadius * 2));
  const lv_font_t* dotFont = peerCount <= 4 ? &lv_font_montserrat_14 : &lv_font_montserrat_12;

  for (uint8_t i = 0; i < kNearbyMaxPeers; i++) {
    lv_obj_set_hidden(dots[i], i >= peerCount);
    if (i >= peerCount) continue;
    int zone = nearbyZone(peers[i].rssi);
    float radius = zone == 0 ? nearRadius : (zone == 1 ? midRadius : farRadius);
    float angle = -90.0f + (360.0f * i / peerCount);
    float radians = angle * DEG_TO_RAD;
    int x = CENTER_X + static_cast<int>(cosf(radians) * radius) - half;
    int y = CENTER_Y + static_cast<int>(sinf(radians) * radius) - half;
    lv_obj_set_pos(dots[i], x, y);
    lv_obj_set_size(dots[i], dotSize, dotSize);
    lv_obj_set_style_text_font(dotText[i], dotFont, 0);
    lv_color_t color = stageColor(peers[i].stage);
    lv_obj_set_style_bg_color(dots[i], color, 0);
    lv_obj_set_style_border_width(dots[i], 0, 0);
    lv_obj_set_style_text_color(dotText[i], peers[i].stage == 0 ? UI_COL_ON_PRIMARY : UI_COL_TEXT, 0);
    char id[5];
    snprintf(id, sizeof(id), "%03X", static_cast<unsigned>(peers[i].id & 0xFFF));
    lv_label_set_text(dotText[i], id);
    focusItems[i] = dots[i];
  }
  if (selected < peerCount) {
    lv_obj_set_size(selectionMarker, dotSize + 8, 4);
    lv_obj_set_pos(selectionMarker,
                   lv_obj_get_x(dots[selected]) - 4,
                   lv_obj_get_y(dots[selected]) + dotSize + 3);
  }
  lv_obj_set_hidden(selectionMarker, selected >= peerCount);
  focusItems[peerCount] = zoomOutBtn;
  focusItems[peerCount + 1] = zoomInBtn;
  focusItems[peerCount + 2] = allBtn;
  uiGroupSet(focusItems, total, false);
  lv_group_focus_obj(focusItems[selected]);

  char status[32];
  if (nearbyIsDemo()) snprintf(status, sizeof(status), "%u pets  " LV_SYMBOL_BULLET "  demo", peerCount);
  else if (!nearbyRadioEnabled()) strlcpy(status, "radio off", sizeof(status));
  else snprintf(status, sizeof(status), "%u pets  " LV_SYMBOL_BULLET "  live", peerCount);
  lv_label_set_text(statusLabel, status);
  lv_obj_set_hidden(emptyLabel, peerCount > 0);
  selectionFill();
}

void sweepTick(lv_timer_t*) {
  if (!sweepArc || !sweepLine) return;
  sweepAngle = fmodf(sweepAngle + 4.0f, 360.0f);
  float radians = sweepAngle * DEG_TO_RAD;
  lv_point_precise_t points[2] = {
    { CENTER_X, CENTER_Y },
    { CENTER_X + static_cast<int>(cosf(radians) * SWEEP_RADIUS),
      CENTER_Y + static_cast<int>(sinf(radians) * SWEEP_RADIUS) }
  };
  lv_line_set_points(sweepLine, points, 2);
  lv_arc_set_angles(sweepArc, static_cast<int>(sweepAngle),
                    static_cast<int>(sweepAngle + 18) % 360);
  for (uint8_t i = 0; i < peerCount; i++) {
    lv_obj_set_style_border_width(dots[i], 0, 0);
  }
}

lv_obj_t* makeControl(lv_obj_t* parent, const char* text, int x, int w) {
  lv_obj_t* btn = lv_obj_create(parent);
  lv_obj_set_size(btn, w, 24);
  lv_obj_set_pos(btn, x, 198);
  uiThemeButton(btn);
  lv_obj_set_style_border_width(btn, 3, LV_STATE_FOCUSED);
  lv_obj_set_style_border_color(btn, UI_COL_TEXT, LV_STATE_FOCUSED);
  lv_obj_set_clickable(btn, true);
  lv_obj_t* label = uiThemeLabel(btn, &lv_font_montserrat_14, UI_COL_TEXT);
  lv_label_set_text(label, text);
  lv_obj_center(label);
  return btn;
}

lv_obj_t* buildNearbyScreen() {
  nearbyScr = lv_obj_create(nullptr);
  uiThemeScreen(nearbyScr);

  lv_obj_t* title = uiThemeLabel(nearbyScr, &lv_font_montserrat_14, UI_COL_EYEBROW);
  lv_label_set_text(title, "NEARBY");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);

  statusLabel = uiThemeLabel(nearbyScr, &lv_font_montserrat_12, UI_COL_MUTED);
  lv_obj_align(statusLabel, LV_ALIGN_TOP_MID, 0, 28);

  selectedLabel = uiThemeLabel(nearbyScr, &lv_font_montserrat_12, UI_COL_TEXT);
  uiThemeTextBounds(selectedLabel, 170, 16);
  lv_obj_set_style_text_align(selectedLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(selectedLabel, LV_ALIGN_TOP_MID, 0, 44);

  const int ringSizes[2] = { 60, 100 };
  for (int i = 0; i < 2; i++) {
    rings[i] = lv_obj_create(nearbyScr);
    lv_obj_set_size(rings[i], ringSizes[i], ringSizes[i]);
    lv_obj_align(rings[i], LV_ALIGN_CENTER, 0, -2);
    lv_obj_set_style_radius(rings[i], LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(rings[i], LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(rings[i], 1, 0);
    lv_obj_set_style_border_color(rings[i], UI_COL_BORDER, 0);
    lv_obj_set_style_border_opa(rings[i], i == 0 ? LV_OPA_40 : LV_OPA_20, 0);
    lv_obj_set_clickable(rings[i], false);
  }

  sweepArc = lv_arc_create(nearbyScr);
  lv_obj_set_size(sweepArc, SWEEP_RADIUS * 2, SWEEP_RADIUS * 2);
  lv_obj_align(sweepArc, LV_ALIGN_CENTER, 0, -2);
  lv_obj_set_style_arc_color(sweepArc, UI_COL_PRIMARY, LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(sweepArc, 2, LV_PART_INDICATOR);
  lv_obj_set_style_arc_opa(sweepArc, LV_OPA_40, LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(sweepArc, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(sweepArc, 0, 0);
  lv_obj_set_clickable(sweepArc, false);

  sweepLine = lv_line_create(nearbyScr);
  lv_obj_set_style_line_width(sweepLine, 2, 0);
  lv_obj_set_style_line_color(sweepLine, UI_COL_PRIMARY, 0);
  lv_obj_set_style_line_opa(sweepLine, LV_OPA_40, 0);
  lv_obj_set_clickable(sweepLine, false);

  nearbyLogo = lv_image_create(nearbyScr);
  lv_image_set_src(nearbyLogo, &nearby_logo);
  lv_image_set_scale(nearbyLogo, 180);
  lv_obj_align(nearbyLogo, LV_ALIGN_CENTER, 0, -2);

  for (uint8_t i = 0; i < kNearbyMaxPeers; i++) {
    dots[i] = lv_obj_create(nearbyScr);
    lv_obj_set_size(dots[i], 28, 28);
    lv_obj_set_style_radius(dots[i], LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(dots[i], LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dots[i], 1, 0);
    lv_obj_set_style_pad_all(dots[i], 0, 0);
    lv_obj_set_clickable(dots[i], true);
    dotText[i] = uiThemeLabel(dots[i], &lv_font_montserrat_12, UI_COL_TEXT);
    lv_obj_center(dotText[i]);
    lv_obj_set_hidden(dots[i], true);
  }

  selectionMarker = lv_obj_create(nearbyScr);
  lv_obj_set_size(selectionMarker, 36, 4);
  lv_obj_set_style_radius(selectionMarker, 2, 0);
  lv_obj_set_style_bg_color(selectionMarker, UI_COL_PRIMARY, 0);
  lv_obj_set_style_bg_opa(selectionMarker, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(selectionMarker, 0, 0);
  lv_obj_set_clickable(selectionMarker, false);
  lv_obj_set_hidden(selectionMarker, true);

  emptyLabel = uiThemeLabel(nearbyScr, &lv_font_montserrat_14, UI_COL_MUTED);
  lv_label_set_text(emptyLabel, "no pets nearby");
  lv_obj_align(emptyLabel, LV_ALIGN_CENTER, 0, 30);

  zoomOutBtn = makeControl(nearbyScr, LV_SYMBOL_MINUS, 54, 28);
  zoomInBtn = makeControl(nearbyScr, LV_SYMBOL_PLUS, 88, 28);
  allBtn = makeControl(nearbyScr, "ALL", 122, 66);
  sweepTimer = lv_timer_create(sweepTick, 50, nullptr);
  return nearbyScr;
}

void zoomBy(int delta) {
  int next = static_cast<int>(zoomPct) + delta;
  zoomPct = static_cast<uint8_t>(next < 75 ? 75 : (next > 125 ? 125 : next));
  refreshPeers();
}

void selectAllPets() {
  if (!peerCount) {
    ui::enqueueToast("no pets nearby");
    return;
  }
  allSelected = true;
  refreshPeers();
  ui::enqueueToast("all pets selected");
}

void sendWave() {
  if (!peerCount) {
    ui::enqueueToast("no pets nearby");
    return;
  }
  if (!nearbySendWave()) {
    ui::enqueueToast("radio off");
    return;
  }
  ui::enqueueToast(allSelected ? "wave sent to all" : "wave sent");
  g_ui.pet->react(PetReaction::REACT_DANCE, millis() / 1000.0f);
}

} // namespace

void uiScreenNearbyShow() {
  bool freeMenu = g_ui.menuScreen && lv_screen_active() == g_ui.menuScreen;
  if (freeMenu) uiScreenMenuShrink();
  if (!nearbyScr) g_ui.nearbyScreen = buildNearbyScreen();
  refreshPeers();
  lv_screen_load_anim(nearbyScr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 160, 0, freeMenu);
  if (freeMenu) uiScreenMenuFreed();
}

void uiScreenNearbyFreed() {
  if (sweepTimer) {
    lv_timer_delete(sweepTimer);
    sweepTimer = nullptr;
  }
  for (uint8_t i = 0; i < kNearbyMaxPeers; i++) {
    dots[i] = nullptr;
    dotText[i] = nullptr;
  }
  nearbyScr = nullptr;
  selectionMarker = nullptr;
  statusLabel = selectedLabel = emptyLabel = nearbyLogo = nullptr;
  rings[0] = rings[1] = nullptr;
  sweepArc = sweepLine = nullptr;
  zoomOutBtn = zoomInBtn = allBtn = nullptr;
  g_ui.nearbyScreen = nullptr;
}

bool uiScreenNearbyActive() {
  return nearbyScr && lv_screen_active() == nearbyScr;
}

void uiScreenNearbyFocusAdvance() {
  uint8_t total = peerCount + 3;
  selected = (selected + 1) % total;
  allSelected = false;
  refreshPeers();
  uiThemeFocusPop(focusItems[selected]);
}

void uiScreenNearbyFocusPrev() {
  uint8_t total = peerCount + 3;
  selected = (selected + total - 1) % total;
  allSelected = false;
  refreshPeers();
  uiThemeFocusPop(focusItems[selected]);
}

void uiScreenNearbyActivate() {
  if (selected < peerCount) sendWave();
  else if (selected == peerCount) zoomBy(-25);
  else if (selected == peerCount + 1) zoomBy(25);
  else if (allSelected) sendWave();
  else selectAllPets();
}

void uiScreenNearbyTick() {
  if (uiScreenNearbyActive()) refreshPeers();
}
