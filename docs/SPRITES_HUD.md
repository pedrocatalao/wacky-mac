# WW.EXE sprites, HUD, main loop — reverse engineering notes

From Ghidra decompilation analysis. Line refs → scratchpad ww_decomp.c.

## CARS.SP frame layout (per kart: 12 frames of 38×28)

- Frames 0–7: rotation views in 45° steps, frame 0 = facing camera,
  frame 4 = rear view. All 8 stored, no mirroring.
- Frames 8–11: hop/jump animation (8/9 rear pair, 10/11 other pair).
- Frame select: octant = round(angle / 240) & 7 for both camera and kart;
  **frame = (kartOct − camOct + 4) mod 8**.
- Spin-out: frame cycles ++ mod 8 per tick. Squash state: 4 frames from
  GENEF.SP.
- Per-character extra .SP files (PANDA/CAMEL/MOOSE/PELICAN/SHARK/RINGO/ELE/
  TIGER.SP): 2+2+8 steering/rear frames + character-specific extras.

## Projection (same math as ground renderer camera)

```
z    = (cos·dx + sin·dy) >> 16              // depth
x'   = (dy·cos − dx·sin) >> 16              // lateral
half = (z · 0x93CD) >> 16                   // tan(30°) Q16 → 60° FOV
sx   = 160 − x'·160/half                    // screen center column
dist = round(isqrt(x'² + z²) · VIEWTAB[sx] / 2^14)   // fisheye Q14
visible: 80 < dist < 1122
scale bucket = clamp((dist − 124)/70, 0..9)          // 10 pre-scaled sizes
row  = 120 + clamp(round(18000/dist), 21, 240)/2     // ground-contact row
```

- Display list, bubble-sorted descending by distance, drawn far→near.
- Player kart: fixed entry, x = 160−w/2, ground row = row(124) = 192,
  dist = 124 (nearest), frame from per-character steering frame list.
- Sprite top clamped to row 110 (below the static backdrop).

## .INF files = precomputed scale tables

Per file 10 records (one per scale bucket):
`{u16 outW, u16 outH, s32 data[outW][outH+1]}` where data[c][0] = start byte
offset of column c in the column-major sprite, data[c][1..outH] = per-output-
pixel source byte steps. Consumed directly by the scaled blitter. 6 files per
race (file 0 = 38X28.INF for karts) + 251X6.INF/ACTION.SP for menus.

## HUD (single player positions)

- Minimap: 78×50 at (0,60); from cup map file (BRONZEM/SILVERM/GOLDM/
  BONUS*.SP, 3900 bytes per track). Dots via MAP.XY = two 2048-entry byte
  tables: mapX = tabX[wx−0x400], mapY = tabY[wy−0x400] + 60. Colors: 0xFF own,
  0x54 human, 0x5A others.
- Race timer "M:SS.T" at (264,112), 7×9 glyphs, 10 Hz counter.
- Speedometer 42×19 at (0,181), digits at (3,183); displayed value eases
  toward 2·speed (+2/+4 up, −4/−10 down per frame).
- Lap icon 22×29 at (25,12); position digits at (181,11); flashing banner
  80×17 at (120,182).
- Split screen: viewport 2 elements at y−100.

## Main loop (race)

```
snapshot 136Hz tick
camera cos/sin + frustum bbox
ground renderer (hidden page)
sprite pass: select frames → project karts/objects/player/effects →
             sort by dist → draw far→near
palette cycling, HUD, animations, input latch
vsync page flip (CRTC start address), page ^= 16000
busy-wait to frame floor (default 12 ticks of 136 Hz)
game logic tick (movement, collisions, laps, AI)
```

## Palette cycling

- Every N.GAM-period frames: indices 168–175 (4 phases) and 161–164 + 148
  (9 phases), source triples from the loaded 768-byte palette (entries 45+).

## Misc files solved

- *.BMC: demo input recordings (u16 count, 3 bytes/frame) for attract mode.
- GIG.MOV: waypoint path {s16 dx, s16 dy} for the "giggle-o-gram" taunt anim.
- WACKY.SDX: per-surface 302-byte records (drag/grip/anims/sound), §PHYSICS.
- WAC1/2.HTT/.OTT: default high-score/best-times tables (written to *.HI).
- SPRITE.ATR record: +0 behavior, +4 w, +6 h, +0xC size class (.INF index),
  +0xE frame count, +0x10 name[20], +0x24 runtime ptr.
- N.SPW: 6 shorts {type, anim(−1 = hidden), x, y, ?, frame}; +0x400 added to
  x,y at load; anim ticks frame++ mod frameCount.
