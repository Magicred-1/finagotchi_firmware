#!/usr/bin/env python3
"""Render wiring_diagram.png: ESP32-S3 + 1.3" ST7789 TFT + 2 buttons.

Pins match src/main.cpp (BUTTON_1_PIN=4, BUTTON_2_PIN=37) and the TFT
build flags in platformio.ini. Buttons wire GPIO -> GND (internal pull-ups).
"""

from PIL import Image, ImageDraw, ImageFont

W, H = 1200, 1230
BG = (236, 234, 226)
INK = (40, 40, 40)

RED = (220, 50, 60)
BLACK = (30, 30, 30)
YELLOW = (240, 200, 40)
GREEN = (60, 160, 80)
WHITE = (245, 245, 245)
ORANGE = (240, 150, 40)
BLUE = (70, 130, 220)
PURPLE = (150, 80, 180)
TEAL = (0, 170, 190)
MAGENTA = (214, 51, 132)
SILVER = (175, 175, 175)

FONT_PATH = "/System/Library/Fonts/Menlo.ttc"


def font(size):
    return ImageFont.truetype(FONT_PATH, size, index=0)


img = Image.new("RGB", (W, H), BG)
d = ImageDraw.Draw(img)


def text(x, y, s, size=20, fill=INK, anchor="mm"):
    d.text((x, y), s, font=font(size), fill=fill, anchor=anchor)


def wire(points, color, width=5):
    d.line(points, fill=color, width=width, joint="curve")


def dot(x, y, color, r=6):
    d.ellipse((x - r, y - r, x + r, y + r), fill=color, outline=(60, 60, 60))


# --- title ---
text(W // 2, 48, 'ESP32-S3 + 1.3" ST7789 TFT + Buttons Wiring', 30)

# --- breadboard rails (decorative) ---
for cx in (104, 1096):
    d.rectangle((cx - 14, 120, cx + 14, 1060), fill=(208, 206, 198), outline=(170, 168, 160))
    text(cx, 140, "+", 22, fill=(200, 60, 60))
    text(cx, 1032, "-", 22, fill=(70, 90, 160))

# --- display module ---
d.rectangle((400, 110, 800, 330), fill=(43, 94, 167), outline=(20, 40, 80), width=2)
d.rectangle((425, 135, 775, 280), fill=(15, 15, 18))
text(600, 96, '1.3" ST7789 TFT (240x240)', 22)

# pin header along the bottom edge of the module
PINS = [  # (x, label, color)
    (420, "VCC", RED),
    (465, "GND", BLACK),
    (510, "SCL", YELLOW),
    (555, "SDA", GREEN),
    (600, "RES", WHITE),
    (645, "DC", ORANGE),
    (690, "CS", BLUE),
    (735, "BL", PURPLE),
]
for x, label, color in PINS:
    text(x, 300, label, 16, fill=(255, 255, 255))
    dot(x, 330, color, 7)

# --- ESP32 board ---
text(600, 790, "ESP32-S3-DevKitC-1", 22)
d.rectangle((380, 810, 820, 980), fill=(45, 45, 48), outline=(20, 20, 20), width=2)
d.rectangle((560, 830, 760, 905), fill=(198, 168, 105), outline=(150, 125, 70))
text(660, 868, "ESP32-S3-WROOM-1", 13, fill=(70, 55, 25))

# top-edge connections (display signals), label = GPIO on the board
TOP = [
    (420, "3V3", RED),
    (465, "GND", BLACK),
    (510, "12", YELLOW),
    (555, "11", GREEN),
    (600, "14", WHITE),
    (645, "9", ORANGE),
    (690, "10", BLUE),
    (735, "13", PURPLE),
]
for x, label, color in TOP:
    wire([(x, 337), (x, 810)], color)
    dot(x, 810, color, 7)
    text(x, 824, label, 14, fill=(255, 255, 255))

# bottom-edge connections
for x, label, color in ((420, "4", TEAL), (465, "GND", BLACK), (780, "37", MAGENTA)):
    dot(x, 980, color, 7)
    text(x, 964, label, 14, fill=(255, 255, 255))

# --- buttons ---
def button(cx, name, sub, wire_color):
    x0, y0, x1, y1 = cx - 55, 700, cx + 55, 800
    # legs (2 per side)
    for ly in (722, 766):
        d.rectangle((x0 - 12, ly, x0, ly + 14), fill=SILVER, outline=(120, 120, 120))
        d.rectangle((x1, ly, x1 + 12, ly + 14), fill=SILVER, outline=(120, 120, 120))
    d.rectangle((x0, y0, x1, y1), fill=(60, 60, 60), outline=(25, 25, 25), width=2)
    d.ellipse((cx - 22, 750 - 22, cx + 22, 750 + 22), fill=(210, 55, 55), outline=(120, 30, 30))
    text(cx, 668, name, 20)
    text(cx, 692, sub, 15, fill=(90, 90, 90))
    return (x0, y0, x1, y1)


b1 = button(175, "Button 1 - action", "to GPIO4 + GND", TEAL)
b2 = button(1025, "Button 2 - navigate", "to GPIO37 + GND", MAGENTA)

# GND rail (common ground)
RAIL_Y = 1050
d.line([(70, RAIL_Y), (1130, RAIL_Y)], fill=BLACK, width=4)
text(600, RAIL_Y + 18, "GND rail - common ground (tie to ESP32 GND)", 15, fill=(90, 90, 90))

# button 1: GPIO wire (right-top leg) -> GPIO4 ; GND wire (left-bottom leg) -> rail
wire([(b1[2] + 12, 729), (340, 729), (340, 1000), (420, 1000), (420, 980)], TEAL)
wire([(b1[0] - 12, 773), (70, 773), (70, RAIL_Y)], BLACK)
dot(70, RAIL_Y, BLACK)

# button 2: GPIO wire (left-top leg) -> GPIO37 ; GND wire (right-bottom leg) -> rail
wire([(b2[0] - 12, 729), (860, 729), (860, 1000), (780, 1000), (780, 980)], MAGENTA)
wire([(b2[2] + 12, 773), (1130, 773), (1130, RAIL_Y)], BLACK)
dot(1130, RAIL_Y, BLACK)

# ESP32 GND -> rail
wire([(465, 980), (465, RAIL_Y)], BLACK)
dot(465, RAIL_Y, BLACK)

# --- footnote + legend ---
text(W // 2, 1092, "Buttons short GPIO to GND when pressed - no resistors (internal pull-ups). Active-LOW.", 16)

LEGEND = [
    ("VCC", RED), ("GND", BLACK), ("SCL", YELLOW), ("SDA", GREEN), ("RES", WHITE),
    ("DC", ORANGE), ("CS", BLUE), ("BL", PURPLE), ("BTN1 GPIO4", TEAL), ("BTN2 GPIO37", MAGENTA),
]
text(70, 1132, "Wire colors:", 18, anchor="lm")
for i, (label, color) in enumerate(LEGEND):
    row, col = divmod(i, 5)
    x = 230 + col * 195
    y = 1132 + row * 36
    d.ellipse((x - 8, y - 8, x + 8, y + 8), fill=color, outline=(60, 60, 60))
    text(x + 16, y, label, 16, anchor="lm")

img.save("wiring_diagram.png")
print("wrote wiring_diagram.png")
