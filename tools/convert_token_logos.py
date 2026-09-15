"""Convert xStocks token logos (token_logos/*.png) to a C header for TFT_eSPI.

Sources are the official 400x400 RGBA token icons from the xStocks metadata
CDN (see https://docs.xstocks.fi/apis/openapi — /api/v2/public/assets gives a
`logo` URL per asset). Processing:

  1. Resize to 48x48 (LANCZOS) — the big logo on the DCA positions card.
  2. Alpha-composite over the scene navy #07111F (TFT_eSPI has no alpha).
  3. Emit RGB565 PROGMEM arrays + a ticker lookup table.

Run: python3 tools/convert_token_logos.py
"""
from PIL import Image
import glob
import os

NAVY = (0x07, 0x11, 0x1F)
SIZE = 48
INPUT_DIR = "token_logos"
OUTPUT = "src/token_logos.h"
PREVIEW_DIR = "token_logos/preview"


def to_array_name(ticker: str) -> str:
    return "logo_" + ticker.lower()


def main():
    entries = []
    for path in sorted(glob.glob(os.path.join(INPUT_DIR, "*.png"))):
        # File names use the canonical xStocks spelling (SPYx.png);
        # firmware tickers are uppercase (SPYX).
        ticker = os.path.splitext(os.path.basename(path))[0].upper()

        img = Image.open(path).convert("RGBA").resize(
            (SIZE, SIZE), Image.Resampling.LANCZOS)
        bg = Image.new("RGB", (SIZE, SIZE), NAVY)
        bg.paste(img, mask=img.getchannel("A"))

        data = []
        for r, g, b in bg.getdata():
            data.append(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3))

        entries.append((ticker, to_array_name(ticker), data))
        os.makedirs(PREVIEW_DIR, exist_ok=True)
        bg.save(os.path.join(PREVIEW_DIR, ticker + ".png"))

    with open(OUTPUT, "w") as f:
        f.write("// Auto-generated from token_logos/*.png\n")
        f.write("// Do not edit by hand. Run: python3 tools/convert_token_logos.py\n\n")
        f.write("#pragma once\n#include <stdint.h>\n\n")
        f.write(f"#define TOKEN_LOGO_SIZE {SIZE}\n\n")
        f.write("struct TokenLogo { const char* ticker; const uint16_t* data; };\n\n")
        for ticker, name, data in entries:
            f.write(f"const uint16_t {name}[{SIZE * SIZE}] PROGMEM = {{\n")
            for i in range(0, len(data), 14):
                f.write("  " + ", ".join(f"0x{v:04X}" for v in data[i:i + 14]) + ",\n")
            f.write("};\n\n")
        f.write("const TokenLogo TOKEN_LOGOS[] = {\n")
        for ticker, name, _ in entries:
            f.write(f'  {{ "{ticker}", {name} }},\n')
        f.write("};\n")
        f.write(f"#define TOKEN_LOGO_COUNT {len(entries)}\n")

    print(f"Wrote {OUTPUT} with {len(entries)} logos "
          f"({len(entries) * SIZE * SIZE * 2} bytes of pixel data)")
    print(f"Previews in {PREVIEW_DIR}/")


if __name__ == "__main__":
    main()
