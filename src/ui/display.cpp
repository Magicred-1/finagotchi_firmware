/*
  display.cpp — LVGL v9 display driver over TFT_eSPI.

  The whole pipeline renders LV_COLOR_FORMAT_RGB565_SWAPPED (see lv_conf.h):
  the panel expects big-endian pixels and the pet's TFT_eSprite framebuffer
  is already big-endian, so LVGL's draw buffer can be pushed to the panel
  verbatim (no byte-swap pass anywhere). Two 240x30 partial draw buffers
  (~28 KB) keep RAM in check; the flush is a blocking pushPixels, same SPI
  cost as the old full-screen pushSprite.
*/

#include "ui_internal.h"

namespace {

constexpr int32_t DISP_W = 240;
constexpr int32_t DISP_H = 240;
constexpr int32_t BUF_LINES = 30;   // RAM fallback: drop to 20 if alloc fails

LV_ATTRIBUTE_MEM_ALIGN uint8_t buf1[DISP_W * BUF_LINES * 2];
LV_ATTRIBUTE_MEM_ALIGN uint8_t buf2[DISP_W * BUF_LINES * 2];

void flushCb(lv_display_t* disp, const lv_area_t* area, uint8_t* pxMap) {
  uint32_t w = static_cast<uint32_t>(area->x2 - area->x1 + 1);
  uint32_t h = static_cast<uint32_t>(area->y2 - area->y1 + 1);
  TFT_eSPI* tft = g_ui.tft;
  tft->setSwapBytes(false);   // buffer is already in wire order
  tft->startWrite();
  tft->setAddrWindow(area->x1, area->y1, w, h);
  tft->pushPixels(pxMap, w * h);
  tft->endWrite();
  lv_display_flush_ready(disp);
}

uint32_t tickCb() { return millis(); }

void logCb(lv_log_level_t level, const char* buf) {
  Serial.printf("LVGL(%d): %s\n", static_cast<int>(level), buf);
}

} // namespace

void uiDisplayInit(TFT_eSPI* tft) {
  g_ui.tft = tft;
  lv_tick_set_cb(tickCb);
  lv_log_register_print_cb(logCb);

  lv_display_t* disp = lv_display_create(DISP_W, DISP_H);
  lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565_SWAPPED);
  lv_display_set_buffers(disp, buf1, buf2, sizeof(buf1),
                         LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(disp, flushCb);
}
