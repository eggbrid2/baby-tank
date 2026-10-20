#!/usr/bin/env python3
"""Extract complete food and drink illustrations from the interaction atlas.

The generated atlas deliberately lets illustrations cross its 256 px guide cells.
Cropping those cells directly clips the next row and leaks it into the previous
row.  This script follows each illustration's transparent connected component
instead, then places the complete result on a fresh 256 px transparent canvas.
"""

from __future__ import annotations

from collections import deque
from dataclasses import dataclass
from pathlib import Path

from PIL import Image, ImageFilter


ROOT = Path(__file__).resolve().parents[1]
ATLAS = ROOT / "android/design-assets/interaction_atlas_v1.png"
ALPHA = ROOT / "android/design-assets/interaction_atlas_alpha.png"
OUTPUT = ROOT / "android/app/src/main/res/drawable-nodpi"
CANVAS_SIZE = 256
CONTENT_SIZE = 226
ALPHA_THRESHOLD = 12


@dataclass(frozen=True)
class Asset:
    filename: str
    seeds: tuple[tuple[int, int], ...]


# The first seed selects the main illustration. Additional seeds retain detached
# comic accents that belong to the same illustration, such as juice splashes and
# hot-cocoa steam.
ASSETS = (
    Asset("food_strawberry.png", ((130, 620),)),
    Asset("food_apple.png", ((390, 620),)),
    Asset("food_banana.png", ((690, 620),)),
    Asset("food_cake.png", ((900, 630),)),
    Asset("food_cookie.png", ((130, 850),)),
    Asset("food_bread.png", ((400, 850),)),
    Asset("food_rice_ball.png", ((650, 830),)),
    Asset("food_ice_cream.png", ((900, 850),)),
    Asset("drink_milk.png", ((130, 1100),)),
    Asset(
        "drink_juice.png",
        ((400, 1120), (302, 1014), (289, 1060), (496, 1007), (514, 1048)),
    ),
    Asset("drink_water.png", ((650, 1100),)),
    Asset("drink_yogurt.png", ((900, 1120),)),
    Asset("drink_soy_milk.png", ((130, 1370),)),
    Asset("drink_cocoa.png", ((390, 1400), (309, 1268), (451, 1273))),
)


def nearest_opaque(alpha: Image.Image, seed: tuple[int, int]) -> tuple[int, int]:
    """Return the closest opaque pixel when a visual seed lands in a hollow."""

    pixels = alpha.load()
    width, height = alpha.size
    seed_x, seed_y = seed
    for radius in range(0, 81):
        left = max(0, seed_x - radius)
        right = min(width - 1, seed_x + radius)
        top = max(0, seed_y - radius)
        bottom = min(height - 1, seed_y + radius)
        for x in range(left, right + 1):
            for y in (top, bottom):
                if pixels[x, y] >= ALPHA_THRESHOLD:
                    return x, y
        for y in range(top + 1, bottom):
            for x in (left, right):
                if pixels[x, y] >= ALPHA_THRESHOLD:
                    return x, y
    raise RuntimeError(f"No opaque pixels found near atlas seed {seed}")


def component_mask(alpha: Image.Image, seeds: tuple[tuple[int, int], ...]) -> Image.Image:
    width, height = alpha.size
    source = alpha.load()
    selected = Image.new("L", alpha.size, 0)
    target = selected.load()

    for requested_seed in seeds:
        seed = nearest_opaque(alpha, requested_seed)
        if target[seed] == 255:
            continue
        queue: deque[tuple[int, int]] = deque((seed,))
        target[seed] = 255
        while queue:
            x, y = queue.popleft()
            for next_x, next_y in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)):
                if not (0 <= next_x < width and 0 <= next_y < height):
                    continue
                if target[next_x, next_y] or source[next_x, next_y] < ALPHA_THRESHOLD:
                    continue
                target[next_x, next_y] = 255
                queue.append((next_x, next_y))

    # Recover the original antialiased fringe around the selected binary region.
    fringe = selected.filter(ImageFilter.MaxFilter(5))
    return Image.composite(alpha, Image.new("L", alpha.size, 0), fringe)


def extract(atlas: Image.Image, alpha: Image.Image, asset: Asset) -> tuple[int, int]:
    mask = component_mask(alpha, asset.seeds)
    bounds = mask.getbbox()
    if bounds is None:
        raise RuntimeError(f"No illustration selected for {asset.filename}")

    illustration = atlas.copy()
    illustration.putalpha(mask)
    illustration = illustration.crop(bounds)
    scale = min(CONTENT_SIZE / illustration.width, CONTENT_SIZE / illustration.height)
    size = (
        max(1, round(illustration.width * scale)),
        max(1, round(illustration.height * scale)),
    )
    illustration = illustration.resize(size, Image.Resampling.LANCZOS)
    canvas = Image.new("RGBA", (CANVAS_SIZE, CANVAS_SIZE), (0, 0, 0, 0))
    position = ((CANVAS_SIZE - size[0]) // 2, (CANVAS_SIZE - size[1]) // 2)
    canvas.alpha_composite(illustration, position)
    canvas.save(OUTPUT / asset.filename, optimize=True)
    return size


def main() -> None:
    atlas = Image.open(ATLAS).convert("RGBA")
    alpha = Image.open(ALPHA).convert("L")
    if atlas.size != alpha.size:
        raise RuntimeError(f"Atlas {atlas.size} and alpha mask {alpha.size} differ")
    for asset in ASSETS:
        size = extract(atlas, alpha, asset)
        print(f"{asset.filename}: content={size[0]}x{size[1]}")


if __name__ == "__main__":
    main()
