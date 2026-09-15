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

Two momentary push buttons for local interaction, wired to GND. The firmware
uses the ESP32's **internal pull-ups**, so each button just shorts its GPIO to
GND when pressed — **no resistors needed**.

### Parts

- 2× tactile push buttons (standard 6×6 mm, 4 legs)
- 4× jumper wires

### How the 4-leg button works

The 4 legs are internally connected in pairs. On each **side** of the button,
the two legs are the same electrical node; the press bridges the two sides:

```
side A ──o   o── side B      (press to connect A and B)
side A ──o   o── side B
```

So you only need **one leg from each side** — pick any leg on side A for GPIO
and any leg on side B for GND.

### Breadboard placement

1. **Straddle the center gap.** Push the button across the breadboard's
   center channel so two legs land on one half (e.g. columns e/f) and two on
   the other half. This guarantees you're actually using both sides of the
   switch. (If all 4 legs are on the same 5-hole strip, the button is
   shorted permanently and the pin reads as always-pressed.)
2. **Wire GND:** jumper from one leg of the button to a GND rail. Tie the
   GND rail to the ESP32's GND (shared ground with the display).
3. **Wire GPIO:** jumper from the leg on the *other side* of the center gap
   to the GPIO pin below.
4. Repeat for the second button, using a different row.

```
        ┌─ button ─┐
GPIO ───┤ e    f   ├─── GND rail ─── GND (ESP32)
        └──────────┘
            ▲ straddles center gap
```

Sanity check: with nothing pressed, the GPIO reads HIGH (3.3 V via internal
pull-up); pressed = LOW. Active-low is what the firmware expects.

### Pin & function map

| Button | GPIO | Function |
|--------|------|----------|
| Button 1 (left, action) | GPIO4 | Short press: context action — feed/play on the pet page (+10 happiness, happy mood), next plan card on the DCA page. Long press (1s): cycle mood. Double press: cycle reaction (jump → spin → glow → dance) |
| Button 2 (right, navigate) | GPIO37 | Short press: switch page (pet ↔ DCA positions). Long press (1s): jump back to the pet page. Double press: resume carousel auto-rotate (drops a pinned plan) |

> GPIO4 is now the left button — the battery voltage divider moved to GPIO5
> (see below).

> On ESP32-S3 modules with **octal PSRAM** (e.g. N16R8), GPIO 33–37 are used
> by the PSRAM bus — GPIO37 won't work as a button there. Use a quad-PSRAM
> module (N8R2) or pick a different pin in that case.

> If your buttons are wired to different pins, edit `BUTTON_1_PIN` and
> `BUTTON_2_PIN` in `src/main.cpp`.

## Battery sense (optional)

Top-right battery icon on screen. LiPo+ → voltage divider → GPIO5 (ADC1 —
ADC2 conflicts with Wi-Fi):

| Connection | Part |
|------------|------|
| LiPo+ → GPIO5 | 2× 100k resistors in series (1:1 divider) |
| Midpoint → GPIO5 | tap between the two resistors |
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
