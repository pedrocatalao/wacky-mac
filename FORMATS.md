# Wacky Wheels (Apogee, 1994) — File Format Notes

Findings from reverse engineering the registered v1.1 data files, cross-checked
against the [ModdingWiki](https://moddingwiki.shikadi.net/wiki/Wacky_Wheels).

The engine (`WW.EXE`) is a 32-bit DOS/4GW linear executable (Watcom C).
Rendering is VGA 320x200 (Mode 13h/Mode X), 256 colors, 6-bit DAC palette.

## WACKY.DAT — archive

```
uint16le file_count            // 577 in registered 1.1
entries[file_count]:
    char[14]  filename         // NUL-padded 8.3
    uint32le  size
    uint32le  offset           // absolute; FILE CONTENT STARTS AT offset+2
```
Files are stored back-to-back, no compression. **CRITICAL QUIRK: each entry's
data begins 2 bytes after its TOC offset** (there are 2 slack bytes between
the TOC/previous file and each file's content). Extracting at `offset` yields
files whose first 2 bytes belong to the previous file and whose last 2 bytes
are missing — which historically produced myths about "2-byte PCX prefixes",
"unmarked palettes", and exotic table encodings. Extractor: `tools/wdat.py`.

## PCX (all *.PCX inside the DAT)

**Completely standard ZSoft PCX**: 8bpp, single plane, linear RLE (runs may
cross row boundaries), `0x0C` marker + 768-byte 8-bit RGB palette at EOF
(the game shifts values >>2 for the 6-bit DAC).

The **race palette comes from the primary tileset** (`a_f<n>.pcx`, .GAM line 1);
backdrops and all race art are displayed through it. Indices reserved for
color cycling: 148, 161–164, 168–175. Converter: `tools/wpcx.py`.

## Tracks — seven files per track, numbered 1–42

Tracks 1–15ish are races; the rest are battle/duck arenas (same format).
World space is 2048x2048 px = 64x64 tiles of 32x32 px.

| File | Contents |
|------|----------|
| `N.GAM` | Text manifest (see below) |
| `N.M`   | 64x64 tile map, 1 byte/tile. Index 0–53 → primary tileset, 54+ → secondary. Tileset PCX is 320x200 sliced into 32x32 tiles, 10 per row, row-major. |
| `N.PAR` | 25600 bytes — parallax/horizon strip data (per ModdingWiki: horizon graphics config) |
| `N.POS` | 64x64 grid of uint16le progress numbers (wrong-way detection; increases along track) |
| `N.RD`  | AI road path: uint16le count, then structs `{x1,y1,x2,y2,angle,compass,dist}` (all uint16le). Angle unit = degrees*16/3 (0–1920, 0=east). |
| `N.SIN` | Per-tile-type properties, 12-byte structs (surface/water flags) |
| `N.SPW` | World objects: uint16le count, then `{objectid,0,x,y,0,anim}` (12 bytes; anim<0 = cycle) |

### N.GAM (plain text, CRLF lines)

```
line 1-2   primary/secondary tileset PCX ("f1.pcx" → prepend "a_" → A_F1.PCX)
line 3-4   minimap tile PCX (ma1.pcx → A_MA1.PCX)
line 5     horizon background PCX (back1.pcx)
line 6,7   1250 / 1500 (constant, unknown)
line 8     "1"
line 9-24  eight (x,y) kart start positions
line 25    color cycling enable
line 26-28 (optional) cycle delays
```

## Sprites

### SPRITE.ATR — object sprite table
```
uint16le total_frame_count?    // 106
entries[36] (40 bytes each):
    uint16le f0..f8            // f3,f4 = width,height; f8 = frame count
                               // (f1 ~ frames-related, f7 ~ type?)
    char[22] filename          // H1.SP..H25.SP, OB1.SP..OB13.SP
```

### *.SP — raw sprite frames
Headerless raw 8bpp frames, concatenated. Frame pixels are stored
**transposed** (rotate 90° CW + h-flip — column rendering optimization).
Transparency = palette index 90 (0x5A).

- `CARS.SP`: 96 frames of 38x28 — 8 karts x 12 rotation angles
  (remaining angles mirrored at runtime).
- `H*.SP`/`OB*.SP`: dimensions from SPRITE.ATR (32x24, 28x28, 14x28).
- Some SP files (e.g. `AJ.SP`) contain ASCII grids — different sub-format, TODO.

### *.INF (named `<W>X<H>.INF`)
Per-dimension sprite metadata: `FFFF, uint16 w, uint16 h`, then repeating
records — likely per-frame offset/advance tables. Not fully decoded yet.

## Audio

- `*.MID` — standard MIDI (16 tracks of music, for GM/GUS).
- `*.KLM` — same songs in AdLib OPL2 format ([KLM Format](https://moddingwiki.shikadi.net/wiki/KLM_Format)).
  Header: `{u16 tempo_ticks_per_s, u8 ?, u16 song_data_offset}`, then 11-byte
  OPL2 instrument records (regs 40/43/60/63/80/83/20/23/E0/E3/C0), then a
  command stream: low nibble = channel; `0x0n` note off, `0x1n`+2B note on
  (bytes → regs A0/B0), `0x16-0x1A` rhythm bits on reg BD, `0x2n`+1B volume
  ((127-v)/2 → carrier reg 40-family), `0x3n`+1B instrument select,
  `0x4n` key-on, `0xFD`+u8 / `0xFE`+u16 delays (ticks), `0xFF` end.
- `*.VOC` — Creative Voice sound effects (36 files).
- `AUDIOHED.FX` + `AUDIOT.FX` — id Software AudioT-style AdLib SFX bundle.
- `WACKY.SDX` — sound index? TODO.

## Engine tables (semantics from WW.EXE disassembly, in progress)

Angle unit: **1920 units = 360°** (0x780), confirmed by wraparound arithmetic
in WW.EXE (e.g. `-18 mod 1920 → +0x76e`) and the .RD angle spec.

- `TRIG.DAT` (15360 B) — **1920 × {s32 cos, s32 sin} in Q16** (with the
  original generator's small rounding quirks — load verbatim).
- `NDIST` (89600 B) — **320 columns × 70 rows × s32, column-major, plain
  integer distances 133→1122** (ground ray-caster; fisheye baked in).
- `VIEW` (1280 B) — 320 × s32 per screen column, **Q14** fisheye factor
  (center 16384 = 1.0, edges ·cos(30°)); used for sprite distance correction.
- `SLP` (7680 B) — 1920 × s32 = tan(angle) Q16 (frustum wedge tests).
- `VEL.TAB`, `VEL2.TAB` (400 B) — 200 × u16 speed curves: 12 HP and 6 HP
  engines. Index moves +10 accel / −4 coast / −8 brake per tick, cap 100.
- `MAP.XY` (4096 B) — two 2048-entry byte tables mapping world x/y to minimap
  pixel coordinates.

## Reverse engineering infrastructure

- `WW.EXE`: 32-bit LE image at offset 0x290fc (after the DOS/4GW stub).
- Ghidra 12.0.1 + ghidra-lx-loader v12.0.1 imports it as `x86:LE:32`,
  937 functions. Full decompile export: scratchpad `ww_decomp.c` (1.5 MB).
- Asset-loading idiom in the decompilation: `FUN_0001418c("NAME")` opens from
  WACKY.DAT, `FUN_00042c68(dest, size, handle)` reads. Table base globals:
  TRIG→`DAT_0007d6a0`, NDIST→`DAT_0007d660`, VEL.TAB→`DAT_0007e8c4`,
  VEL2.TAB→`DAT_0007e584`, CARS.SP→`DAT_0007d708`, camera angle→`DAT_0007d5d4`.

## Misc / TODO

- `GIG.MOV`, `*.BMC` (small, irregular), `WAC1/2.HTT/.OTT`, `WACKY.SDX`,
  `WWREG.BIN`/`WWSW.BIN` — undecoded.
- `WACKY.ING` — version/config blob ("1.1", "MAR 1994").
- `1.BMC`… numbering matches tracks 1–5,7 only.
