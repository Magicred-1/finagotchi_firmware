# Finagotchi Display Wiring

## Parts
- **ESP32-S3 dev board** (DevKitC-style, dual USB-C ports)
- **1.28" round GC9A01 TFT module** (240×240, SPI — the Waveshare module,
  also sold by The Pi Hut). This is the panel the PCB v1 is designed for and
  the default in `platformio.ini`. The old square ST7789 240×240 module
  still works with a one-line driver change — see "Changing pins or driver".

## Breadboard wiring (GC9A01 module)

The module's silkscreen order, top to bottom:

| Module pin | Connect to ESP32-S3 | GPIO used |
|------------|---------------------|-----------|
| VCC        | 3.3V                | —         |
| GND        | GND                 | —         |
| DIN        | GPIO11              | TFT_MOSI  |
| CLK        | GPIO12              | TFT_SCLK  |
| CS         | GPIO10              | TFT_CS    |
| DC         | GPIO9               | TFT_DC    |
| RST        | GPIO14              | TFT_RST   |
| BL         | GPIO13              | TFT_BL    |

Notes:

- Power from **3.3V**, not 5V: the module accepts 3.3–5 V on VCC but the
  logic is 3.3 V, so keep the whole chain at one level.
- The module ships with a **PH2.0 8-pin cable**; the ESP32 side is 2.54 mm
  Dupont. Either crimp/solder a Dupont plug onto the included cable or
  solder pin headers to the module's edge pads.
- It's a **round** panel: corners of the (square-designed) UI get
  physically clipped. Cosmetic only for now — a circular safe-area pass on
  the UI is part of the PCB v1 work.
- If red/blue ever look swapped on a clone panel, flip
  `-D TFT_RGB_ORDER=TFT_BGR` to `TFT_RGB` in `platformio.ini`.

## On the PCB v1 (Finagotchi_V1)

The on-board display header **J3 (1×8, 2.54 mm)** is the module's pin order
one-for-one, so the Waveshare module plugs straight in:

```
1 VCC(3V3) · 2 GND · 3 DIN · 4 CLK · 5 CS · 6 DC · 7 RST · 8 BL
```

Same SPI GPIOs as the breadboard (DIN 11, CLK 12, CS 10, DC 9, RST 14) —
the one difference is the backlight: **BL is GPIO21 on the PCB** (GPIO13 on
the breadboard). The PCB also moves input to a crown encoder
(A/B/push = GPIO15/16/4) plus a Cancel button (GPIO7); that's handled by the
separate `pcb-v1` PlatformIO environment, not this one.

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
| Either button | GPIO37 (right), GPIO4 (left) | **Both buttons carry the same mapping** — the unit is flaky (captures have shown each button electrically dead at different times), so the scheme is fully redundant and the UI works with whichever button is alive. **Short press = navigate**: walks everything pet → portfolio → menu → pet (moves card/row focus within portfolio/menu first — the menu is a vertical app list, entered on a back-arrow affordance (→) in the right zone, then the apps top-to-bottom; on the create-plan wheel: next option, wrapping; on a detail view: closes it). **Long press = action** for the current screen: sync on the pet screen, open the focused card on portfolio, **pause/resume the plan on the detail view**, run the focused app on the menu (Feed pet / Accessory / Mood / Open DCA, or the back arrow → pet), confirm the drum / CREATE on the create-plan wheel. **Double press on the detail view** (either button): also pause/resume — a single press still closes. Left-button double extras: cycle reaction on the pet screen, toggle amounts USD ↔ SOL on portfolio |

> Redundant scheme: whichever button is alive drives the whole device — short
> to move, long to act, and screens cycle so home is always a few presses
> away. The UI shows no button hints — state captions only.

> **Back / cancel:** double-press the RIGHT button (GPIO37) anywhere —
> closes the detail view, leaves the current screen (the menu's cosmetics
> page steps back to the apps page first, the create-plan wheel steps back
> one drum), and cancels the parental code pad. No-op on the pet screen
> (already home). The LEFT button's double keeps its bonus actions
> (reaction cycle / USD ↔ SOL / pause toggle).

> **Recovery chord:** hold BOTH buttons while powering on to erase all BLE
> bonds from the device AND clear the parental code (splash shows "bonds +
> code erased"). Do this when the app fails to pair after a re-flash — a
> stale bond on either side fails authentication silently and no fresh
> passkey appears (also forget the device in the phone's Bluetooth
> settings) — or when the parental code is forgotten.

> On ESP32-S3 modules with **octal PSRAM** (e.g. N16R8), GPIO 33–37 are used
> by the PSRAM bus — GPIO37 won't work as a button there. Use a quad-PSRAM
> module (N8R2) or pick a different pin in that case.

> If your buttons are wired to different pins, edit `BUTTON_1_PIN` and
> `BUTTON_2_PIN` in `src/ui/input.cpp`.

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

> Some vendor modules label the SPI pins `SCL`/`SDA` — that is clock/data
> naming, not I²C. `SCL` = CLK = SCK, `SDA` = DIN = MOSI.

## Changing pins or driver

Edit `platformio.ini` `build_flags`:

```ini
; Driver options (pick one):
-D GC9A01_DRIVER=1          ; round 240x240 (default — the PCB v1 panel)
; -D ST7789_DRIVER=1        ; legacy square breadboard module
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
-D TFT_BL=13                ; GPIO21 on the PCB v1
```

## If your display is I2C OLED instead (SSD1306 / SH1106)

The wiring and code are different — let me know and I’ll switch it to an I2C OLED setup.
