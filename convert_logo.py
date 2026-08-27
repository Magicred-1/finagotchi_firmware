"""Convert finagotchi_logo.png to a C header for TFT_eSPI.

Processing steps:
  1. Flood-fill the white background (connected to the image border) to black,
     so white letters INSIDE the dark outline are preserved.
  2. Remove the bright anti-aliased fringe ring around the outline.
  3. Crop to the artwork bounding box.
  4. Resize to fit the 240x240 display (LANCZOS).
  5. Emit RGB565 C array.
"""
from PIL import Image, ImageDraw
import sys

INPUT = "finagotchi_logo.png"
OUTPUT = "src/logo.h"
MAX_W, MAX_H = 236, 160     # fits 240x240 with a small margin
FLOOD_THRESH = 60           # tolerance for "white" during flood fill
FRINGE_LEVEL = 200          # pixels brighter than this next to background get cut


def remove_background(img: Image.Image) -> Image.Image:
    w, h = img.size
    seeds = [
        (0, 0), (w - 1, 0), (0, h - 1), (w - 1, h - 1),
        (w // 2, 0), (w // 2, h - 1), (0, h // 2), (w - 1, h // 2),
    ]
    for s in seeds:
        ImageDraw.floodfill(img, s, (0, 0, 0), thresh=FLOOD_THRESH)

    # De-fringe: bright pixels touching the (now black) background -> black.
    px = img.load()
    for y in range(h):
        for x in range(w):
            r, g, b = px[x, y]
            if min(r, g, b) < FRINGE_LEVEL:
                continue
            # has a pure-background neighbor?
            for nx, ny in ((x-1, y), (x+1, y), (x, y-1), (x, y+1)):
                if 0 <= nx < w and 0 <= ny < h:
                    nr, ng, nb = px[nx, ny]
                    if nr == 0 and ng == 0 and nb == 0:
                        px[x, y] = (0, 0, 0)
                        break
    return img


def main():
    try:
        img = Image.open(INPUT).convert("RGB")
    except FileNotFoundError:
        print(f"Error: {INPUT} not found. Save the logo image to the project folder.")
        sys.exit(1)

    img = remove_background(img)

    bbox = img.getbbox()
    if bbox:
        img = img.crop(bbox)

    img.thumbnail((MAX_W, MAX_H), Image.Resampling.LANCZOS)
    w, h = img.size
    print(f"Logo resized to {w}x{h}")

    pixels = list(img.getdata())
    data = []
    for r, g, b in pixels:
        rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        data.append(rgb565)

    with open(OUTPUT, "w") as f:
        f.write("// Auto-generated from finagotchi_logo.png\n")
        f.write("// Do not edit by hand. Run: python3 convert_logo.py\n\n")
        f.write("#pragma once\n")
        f.write("#include <stdint.h>\n\n")
        f.write(f"const uint16_t FINAGOTCHI_LOGO_WIDTH = {w};\n")
        f.write(f"const uint16_t FINAGOTCHI_LOGO_HEIGHT = {h};\n")
        f.write(f"const uint16_t finagotchi_logo[{w * h}] PROGMEM = {{\n")
        for i in range(0, len(data), 12):
            chunk = ", ".join(f"0x{v:04X}" for v in data[i:i+12])
            f.write(f"  {chunk},\n")
        f.write("};\n")

    print(f"Wrote {OUTPUT} ({w * h * 2} bytes)")

    # Also dump a preview so you can check the result before flashing.
    img.save("logo_preview.png")
    print("Wrote logo_preview.png (check this before flashing)")


if __name__ == "__main__":
    main()
