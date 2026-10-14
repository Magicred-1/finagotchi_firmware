"""Convert the mobile app's flower icon to an RGB565 TFT_eSPI header."""
from PIL import Image, ImageChops
import sys

INPUT = "finagotchi_logo.png"
OUTPUT = "src/logo.h"
NAVY = (0x0B, 0x28, 0x58)     # landing.navy / app.json splash color
MAX_W, MAX_H = 120, 120
CROP_MARGIN = 16              # source pixels; preserves the outline antialiasing


def prepare_logo(img: Image.Image) -> Image.Image:
    rgba = img.convert("RGBA")
    bg = Image.new("RGBA", rgba.size, NAVY + (255,))
    bg.alpha_composite(rgba)
    rgb = bg.convert("RGB")

    backdrop = Image.new("RGB", rgb.size, NAVY)
    bbox = ImageChops.difference(rgb, backdrop).getbbox()
    if bbox:
        left = max(0, bbox[0] - CROP_MARGIN)
        top = max(0, bbox[1] - CROP_MARGIN)
        right = min(rgb.width, bbox[2] + CROP_MARGIN)
        bottom = min(rgb.height, bbox[3] + CROP_MARGIN)
        rgb = rgb.crop((left, top, right, bottom))
    return rgb


def main():
    try:
        img = Image.open(INPUT)
    except FileNotFoundError:
        print(f"Error: {INPUT} not found. Save the logo image to the project folder.")
        sys.exit(1)

    img = prepare_logo(img)
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
