#!/usr/bin/env python3
"""Rip Wacky Wheels .SP sprites (raw 8bpp frames, stored rotated) to PNG montages."""
import sys
from pathlib import Path

from PIL import Image

import wpcx


def rip(sp_path: Path, w: int, h: int, pal: bytes, out: Path, transposed=True):
    data = sp_path.read_bytes()
    frame_bytes = w * h
    nframes = len(data) // frame_bytes
    cols = min(nframes, 16)
    rows = (nframes + cols - 1) // cols
    sheet = Image.new("P", (cols * (w + 2), rows * (h + 2)), 0)
    sheet.putpalette(pal)
    for i in range(nframes):
        raw = data[i * frame_bytes : (i + 1) * frame_bytes]
        if transposed:
            # stored column-major: rotate 90 CW + hflip == transpose
            f = Image.frombytes("P", (h, w), raw).transpose(Image.Transpose.TRANSPOSE)
        else:
            f = Image.frombytes("P", (w, h), raw)
        sheet.paste(f, ((i % cols) * (w + 2) + 1, (i // cols) * (h + 2) + 1))
    sheet.putpalette(pal)
    sheet.convert("RGB").save(out)
    print(f"{sp_path.name}: {nframes} frames {w}x{h} (leftover {len(data)%frame_bytes}) -> {out}")


if __name__ == "__main__":
    src = Path(sys.argv[1])
    w, h = int(sys.argv[2]), int(sys.argv[3])
    pal_img = wpcx.decode(Path(sys.argv[4]).read_bytes())
    pal = bytes(pal_img.getpalette())
    transposed = len(sys.argv) < 7 or sys.argv[6] != "flat"
    rip(src, w, h, pal, Path(sys.argv[5]), transposed)
