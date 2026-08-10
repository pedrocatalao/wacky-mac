#!/usr/bin/env python3
"""Extractor/packer for Wacky Wheels WACKY.DAT archives.

Format:
    uint16le  file_count
    entries[file_count]:
        char[14]  filename (NUL-padded 8.3)
        uint32le  size
        uint32le  offset (absolute, from start of file)
    ... file data ...
"""
import struct
import sys
from pathlib import Path

ENTRY = struct.Struct("<14sII")


def read_toc(data: bytes):
    (count,) = struct.unpack_from("<H", data, 0)
    entries = []
    pos = 2
    for _ in range(count):
        raw_name, size, offset = ENTRY.unpack_from(data, pos)
        name = raw_name.split(b"\0", 1)[0].decode("ascii")
        entries.append((name, size, offset))
        pos += ENTRY.size
    return entries


def extract(dat_path: Path, out_dir: Path):
    data = dat_path.read_bytes()
    entries = read_toc(data)
    out_dir.mkdir(parents=True, exist_ok=True)
    for name, size, offset in entries:
        # file content starts 2 bytes after the TOC offset
        (out_dir / name).write_bytes(data[offset + 2 : offset + 2 + size])
    print(f"extracted {len(entries)} files to {out_dir}")


def list_toc(dat_path: Path):
    entries = read_toc(dat_path.read_bytes())
    for name, size, offset in entries:
        print(f"{name:14s} {size:9d} @ 0x{offset:08x}")
    print(f"-- {len(entries)} files")


if __name__ == "__main__":
    if len(sys.argv) < 3 or sys.argv[1] not in ("x", "l"):
        print("usage: wdat.py x <WACKY.DAT> <outdir>  |  wdat.py l <WACKY.DAT> -")
        sys.exit(1)
    if sys.argv[1] == "l":
        list_toc(Path(sys.argv[2]))
    else:
        extract(Path(sys.argv[2]), Path(sys.argv[3]))
