#!/usr/bin/env python3
"""Scan a binary for embedded VGA palettes: 768-byte runs of 6-bit values
with enough distinct colors to be a real palette."""
import sys
from pathlib import Path

data = Path(sys.argv[1]).read_bytes()
i, n = 0, len(data)
hits = []
while i < n - 768:
    if data[i] > 0x3F:
        i += 1
        continue
    # count how far the 6-bit run extends
    j = i
    while j < n and data[j] <= 0x3F:
        j += 1
    if j - i >= 768:
        for start in range(i, j - 767, 768):
            chunk = data[start : start + 768]
            colors = {tuple(chunk[k : k + 3]) for k in range(0, 768, 3)}
            if len(colors) > 64:
                hits.append((start, len(colors)))
    i = j
for off, ncol in hits:
    print(f"offset 0x{off:06x}: {ncol} distinct colors")
print(f"-- {len(hits)} candidate palettes")
