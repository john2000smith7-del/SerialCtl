from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image, ImageDraw


def content_square(image: Image.Image) -> tuple[int, int, int, int]:
    gray = image.convert("L")
    pixels = gray.load()
    width, height = gray.size
    xs: list[int] = []
    ys: list[int] = []
    for y in range(height):
        for x in range(width):
            if pixels[x, y] < 105:
                xs.append(x)
                ys.append(y)
    if not xs:
        # Light-theme artwork may not contain a dark plate. When it is already
        # square, keep the complete composition and only apply the icon mask.
        if width == height:
            return 0, 0, width, height
        raise ValueError("Could not locate the icon tile in the source image")
    left, top, right, bottom = min(xs), min(ys), max(xs) + 1, max(ys) + 1
    side = max(right - left, bottom - top)
    center_x = (left + right) // 2
    center_y = (top + bottom) // 2
    left = max(0, center_x - side // 2)
    top = max(0, center_y - side // 2)
    right = min(width, left + side)
    bottom = min(height, top + side)
    left = right - side
    top = bottom - side
    return left, top, right, bottom


def make_icon(source: Path, png_path: Path, ico_path: Path) -> None:
    original = Image.open(source).convert("RGBA")
    cropped = original.crop(content_square(original))

    # The supplied artwork is a rounded square on white. Keep the artwork exact
    # and only replace the outer white canvas with a clean antialiased alpha mask.
    scale = 4
    large_size = cropped.width * scale
    mask_large = Image.new("L", (large_size, large_size), 0)
    radius = round(large_size * 0.235)
    ImageDraw.Draw(mask_large).rounded_rectangle(
        (0, 0, large_size - 1, large_size - 1), radius=radius, fill=255
    )
    mask = mask_large.resize(cropped.size, Image.Resampling.LANCZOS)
    cropped.putalpha(mask)

    master = cropped.resize((512, 512), Image.Resampling.LANCZOS)
    png_path.parent.mkdir(parents=True, exist_ok=True)
    master.save(png_path, "PNG", optimize=True)
    master.save(
        ico_path,
        "ICO",
        sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)],
    )


def main() -> None:
    parser = argparse.ArgumentParser(description="Build the SerialCtl Windows application icon")
    parser.add_argument("source", type=Path)
    parser.add_argument("png", type=Path)
    parser.add_argument("ico", type=Path)
    args = parser.parse_args()
    make_icon(args.source, args.png, args.ico)


if __name__ == "__main__":
    main()
