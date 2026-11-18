#!/usr/bin/env python3
"""Build the GeoCoin Finder installer with a high-DPI background."""

import argparse
from pathlib import Path
import tempfile

import dmgbuild
from PIL import Image, ImageDraw, ImageFilter, ImageFont

WIDTH = 680
HEIGHT = 420
ARTWORK = Path(__file__).resolve().parents[1] / "src/qt/res/images/background_dark.png"


def render_background(directory):
    scale = 2
    size = WIDTH * scale, HEIGHT * scale
    backdrop = Image.new("RGB", size, (11, 15, 21))
    art_size = 400 * scale
    with Image.open(ARTWORK) as image:
        artwork = image.convert("RGB").resize((art_size, art_size), Image.Resampling.LANCZOS)
    fade = Image.new("L", artwork.size)
    ImageDraw.Draw(fade).ellipse((38, 32, art_size - 38, art_size - 32), fill=174)
    backdrop.paste(artwork, ((size[0] - art_size) // 2, -80), fade.filter(ImageFilter.GaussianBlur(38)))

    def points(coordinates):
        return tuple(coordinate * scale for coordinate in coordinates)

    draw = ImageDraw.Draw(backdrop, "RGBA")
    draw.rounded_rectangle(points((6, 6, WIDTH - 7, 326)), radius=12 * scale,
                           outline=(216, 171, 81, 192), width=scale)
    draw.rounded_rectangle(points((10, 10, WIDTH - 11, 322)), radius=9 * scale,
                           outline=(216, 171, 81, 42), width=scale)

    for x in (150, 530):
        draw.rounded_rectangle(points((x - 82, 94, x + 82, 242)), radius=15 * scale,
                               fill=(12, 17, 24, 217), outline=(218, 171, 83, 144), width=scale)
        draw.rounded_rectangle(points((x - 78, 208, x + 78, 237)), radius=9 * scale,
                               fill=(231, 197, 124, 238))

    # Separate the directional cue from the gold in the coin artwork.
    draw.rounded_rectangle(points((251, 127, 429, 186)), radius=13 * scale,
                           fill=(7, 9, 14, 225), outline=(218, 171, 83, 127), width=scale)
    arrow = ((271, 151), (375, 151), (375, 138), (411, 157),
             (375, 176), (375, 163), (271, 163))
    draw.polygon([points(point) for point in arrow], fill=(239, 189, 83, 255))
    draw.line([points(point) for point in arrow[:4]],
              fill=(255, 229, 168, 255), width=scale)

    caption = ImageFont.truetype("/System/Library/Fonts/SFNS.ttf", 16 * scale)
    draw.rounded_rectangle(points((223, 286, 457, 317)), radius=11 * scale,
                           fill=(9, 13, 20, 224), outline=(170, 133, 61, 126), width=scale)
    draw.text(points((340, 301)), "Drag to Applications", font=caption,
              anchor="mm", fill=(247, 227, 189, 255))

    standard = directory / "background.png"
    backdrop.save(directory / "background@2x.png")
    backdrop.resize((WIDTH, HEIGHT), Image.Resampling.LANCZOS).save(standard)
    return standard


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("app", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    app = args.app.resolve()
    with tempfile.TemporaryDirectory(prefix="geocoin-dmg-layout-") as temporary:
        background = render_background(Path(temporary))
        dmgbuild.build_dmg(str(args.output.resolve()), "GeoCoin Core", settings={
            "format": "UDZO",
            "filesystem": "HFS+",
            "background": str(background),
            "icon": str(app / "Contents/Resources/GeoCoin.icns"),
            "files": [str(app)],
            "symlinks": {"Applications": "/Applications"},
            "window_rect": ((320, 140), (WIDTH, HEIGHT)),
            "icon_locations": {app.name: (150, 157), "Applications": (530, 157)},
            "icon_size": 102,
            "text_size": 13,
        })


if __name__ == "__main__":
    main()
