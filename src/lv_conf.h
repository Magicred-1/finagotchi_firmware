/*
  LVGL v9 configuration for the Finagotchi firmware (240x240 RGB565 ST7789).

  Anything left out falls back to the defaults in lv_conf_internal.h — only
  the settings this firmware actually cares about are pinned here:
  - RGB565_SWAPPED render format: the TFT_eSprite framebuffer AND the
    token_logo PROGMEM arrays are both stored byte-swapped (SPI wire order),
    so rendering swapped end-to-end makes the pet canvas and the logo images
    straight copies and lets the flush push bytes verbatim (no swap passes).
  - heap via the C library (shares the ESP32 heap with BLE/Wi-Fi instead of
    a fixed LVGL pool)
  - log routed through a print callback (registered in ui/display.cpp)
  - Montserrat sizes used by the chrome widgets
*/

#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_FORMAT_DEFAULT LV_COLOR_FORMAT_RGB565_SWAPPED

/* Memory: straight to malloc/free (ESP32 heap). LV_MEM_SIZE is unused then. */
#define LV_USE_STDLIB_MALLOC    LV_STDLIB_CLIB
#define LV_USE_STDLIB_STRING    LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF   LV_STDLIB_CLIB

/* Logging: ui/display.cpp registers a Serial print callback. */
#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 0
#define LV_LOG_USE_TIMESTAMP 0
#define LV_LOG_USE_FILE_LINE 1

/* Default dark theme fits the navy scene; accent gets overridden per-widget. */
#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK 1
#define LV_FONT_DEFAULT LV_FONT_DEFAULT_MONTSERRAT_14

/* Montserrat sizes used by the chrome (each enabled size costs flash). */
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_28 1

/* Widgets the UI layer uses. */
#define LV_USE_CANVAS  1
#define LV_USE_IMAGE   1
#define LV_USE_LABEL   1
#define LV_USE_BAR     1
#define LV_USE_ARC     1
#define LV_USE_SPINNER 1
#define LV_USE_BUTTON  1

#endif /* LV_CONF_H */
