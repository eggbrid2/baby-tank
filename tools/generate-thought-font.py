#!/usr/bin/env python3
"""Generate the tiny antialiased CJK subset used by the sleep bubble."""

from pathlib import Path
import argparse

from PIL import Image, ImageDraw, ImageFont


GLYPHS = "亮晶做个好梦软绵想抱花开啦去探险出玩滚一小鱼游吹泡哼曲"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("font", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    font = ImageFont.truetype(str(args.font), 18, index=2)
    lines = [
        "/* 18x18 4-bit alpha subset of Noto Sans CJK SC Regular (SIL OFL 1.1). */",
        "static const uint8_t s_thought_glyph_alpha[][18][9] = {",
    ]
    for glyph in GLYPHS:
        image = Image.new("L", (18, 18), 0)
        draw = ImageDraw.Draw(image)
        bbox = font.getbbox(glyph)
        draw.text((-bbox[0], -bbox[1]), glyph, font=font, fill=255)
        rows = []
        for y in range(18):
            packed = []
            for x in range(0, 18, 2):
                high = (image.getpixel((x, y)) + 8) // 17
                low = (image.getpixel((x + 1, y)) + 8) // 17
                packed.append((high << 4) | low)
            rows.append("{" + ",".join(f"0x{value:02x}" for value in packed) + "}")
        lines.append(f"    /* {glyph} */ {{" + ",".join(rows) + "},")
    lines.append("};")
    args.output.write_text("\n".join(lines) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
