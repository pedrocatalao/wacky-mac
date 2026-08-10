#!/usr/bin/env python3
"""Convert Wacky Wheels PCX files (standard ZSoft PCX 8bpp) to PNG."""
import struct
import sys
from pathlib import Path

from PIL import Image


def decode(data: bytes):
    if data[0] != 0x0A:
        raise ValueError("not a PCX (bad extraction alignment?)")
    xmin, ymin, xmax, ymax = struct.unpack_from("<4H", data, 4)
    w, h = xmax - xmin + 1, ymax - ymin + 1
    (bpl,) = struct.unpack_from("<H", data, 66)
    need = bpl * h
    pixels = bytearray()
    i = 128
    while len(pixels) < need and i < len(data):
        b = data[i]
        i += 1
        if b & 0xC0 == 0xC0:
            pixels += bytes([data[i]]) * (b & 0x3F)
            i += 1
        else:
            pixels.append(b)
    pal = data[-768:]
    img = Image.frombytes("P", (bpl, h), bytes(pixels[:need])).crop((0, 0, w, h))
    img.putpalette(pal)
    return img


def convert(src: Path, dst: Path) -> str:
    img = decode(src.read_bytes())
    img.save(dst)
    return f"{src.name}: {img.size[0]}x{img.size[1]}"


if __name__ == "__main__":
    out_dir = Path(sys.argv[-1])
    out_dir.mkdir(parents=True, exist_ok=True)
    ok = bad = 0
    for arg in sys.argv[1:-1]:
        src = Path(arg)
        try:
            convert(src, out_dir / (src.stem + ".png"))
            ok += 1
        except Exception as e:
            print(f"{src.name}: FAILED ({e})", file=sys.stderr)
            bad += 1
    print(f"-- converted {ok}, failed {bad}")
