# Finagotchi Display Wiring

## Parts in your photo
- **ESP32-S3 dev board** (DevKitC-style, dual USB-C ports)
- **1.3"/1.54" SPI TFT module** (default config assumes ST7789 240×240)

## Breadboard wiring

| Display pin | Connect to ESP32-S3 | GPIO used |
|-------------|---------------------|-----------|
| VCC / 3.3V  | 3.3V                | —         |
| GND         | GND                 | —         |
| SCL / SCK   | GPIO12              | TFT_SCLK  |
| SDA / MOSI  | GPIO11              | TFT_MOSI  |
| RES / RST   | GPIO14              | TFT_RST   |
| DC          | GPIO9               | TFT_DC    |
| CS          | GPIO10              | TFT_CS    |
| BL / LED    | GPIO13 (optional)   | TFT_BL    |

## Buttons

Two momentary push buttons for local interaction. One leg to GPIO, other leg
to GND rail. Uses internal pull-ups (no external resistor needed).

| Button | GPIO | Function |
|--------|------|----------|
| Button 1 | GPIO37 | Short press: cycle reaction (jump → spin → glow → dance). Long press (1s): cycle mood |
| Button 2 | GPIO39 | Short press: feed/play (+10 happiness, happy mood). Long press (1s): cycle mood |

> On ESP32-S3 modules with **octal PSRAM** (e.g. N16R8), GPIO 33–37 are used
> by the PSRAM bus — GPIO37 won't work as a button there. Use a quad-PSRAM
> module (N8R2) or pick a different pin in that case.

> If your buttons are wired to different pins, edit `BUTTON_1_PIN` and
> `BUTTON_2_PIN` in `src/main.cpp`.

## Battery sense (optional)

Top-right battery icon on screen. LiPo+ → voltage divider → GPIO4 (ADC1 —
ADC2 conflicts with Wi-Fi):

| Connection | Part |
|------------|------|
| LiPo+ → GPIO4 | 2× 100k resistors in series (1:1 divider) |
| Midpoint → GPIO4 | tap between the two resistors |
| LiPo− | GND rail (shared with the board!) |

Without the divider the icon hides itself (reads ~0 V = USB power assumed).
> Never connect LiPo+ directly to a GPIO — 4.2 V exceeds the 3.3 V max.

> If your display module has pins labeled `SCL`/`SDA`, that is the SPI clock/data naming used by some vendors. `SCL` = SCK, `SDA` = MOSI.

## Changing pins or driver

Edit `platformio.ini` `build_flags`:

```ini
; Driver options (pick one):
-D ST7789_DRIVER=1
; -D GC9A01_DRIVER=1
; -D ILI9341_DRIVER=1

; Resolution
-D TFT_WIDTH=240
-D TFT_HEIGHT=240

; Pin numbers
-D TFT_MOSI=11
-D TFT_SCLK=12
-D TFT_CS=10
-D TFT_DC=9
-D TFT_RST=14
-D TFT_BL=13
```

## If your display is I2C OLED instead (SSD1306 / SH1106)

The wiring and code are different — let me know and I’ll switch it to an I2C OLED setup.
