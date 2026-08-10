#!/usr/bin/env python3
"""Render a Wacky Wheels track (.M map + tileset PCXs) to a 2048x2048 PNG."""
import sys
from pathlib import Path

from PIL import Image

import wpcx

TILE = 32
MAP_W = MAP_H = 64
TILES_PER_ROW = 10  # 320px tileset / 32px tiles


def load_gam(gam_path: Path):
    lines = gam_path.read_bytes().split(b"\r\n")
    txt = [l.decode("ascii", "replace").strip() for l in lines]
    return {
        "tileset1": "a_" + txt[0],
        "tileset2": "a_" + txt[1],
        "back": txt[4],
        "starts": [(int(txt[8 + i * 2]), int(txt[9 + i * 2])) for i in range(8)],
    }


def tile_image(tilesets, idx):
    if idx < 54:
        src, j = tilesets[0], idx
    else:
        src, j = tilesets[1], idx - 54
    x, y = (j % TILES_PER_ROW) * TILE, (j // TILES_PER_ROW) * TILE
    return src.crop((x, y, x + TILE, y + TILE))


def render(track_dir: Path, num: int, out_path: Path):
    gam = load_gam(track_dir / f"{num}.GAM")
    ts1 = wpcx.decode((track_dir / gam["tileset1"].upper()).read_bytes()).convert("RGB")
    ts2 = wpcx.decode((track_dir / gam["tileset2"].upper()).read_bytes()).convert("RGB")
    m = (track_dir / f"{num}.M").read_bytes()
    world = Image.new("RGB", (MAP_W * TILE, MAP_H * TILE))
    cache = {}
    for ty in range(MAP_H):
        for tx in range(MAP_W):
            idx = m[ty * MAP_W + tx]
            if idx not in cache:
                cache[idx] = tile_image((ts1, ts2), idx)
            world.paste(cache[idx], (tx * TILE, ty * TILE))
    world.save(out_path)
    print(f"track {num}: {sorted(set(m))[:8]}... {len(set(m))} distinct tiles -> {out_path}")


if __name__ == "__main__":
    track_dir = Path(sys.argv[1])
    out_dir = Path(sys.argv[3])
    out_dir.mkdir(parents=True, exist_ok=True)
    for num in sys.argv[2].split(","):
        render(track_dir, int(num), out_dir / f"track{num}.png")
