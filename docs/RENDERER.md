# WW.EXE ground renderer — reverse engineering notes

Source: Ghidra decompilation analysis (line refs → scratchpad ww_decomp.c).
This is the authoritative spec for the faithful renderer reimplementation.

## Core algorithm — column ray-caster (NOT scanline Mode 7)

`FUN_00037e94` (bottom view, rows 130–199) / `FUN_0003c3e9` (split-screen top
view, rows 30–99); each has 3 detail-level branches.

High detail, per frame:

```c
a = camera_angle - 160;  if (a < 0) a += 1920;   // leftmost ray
ray = &TRIG[a];                                   // {s32 cosQ16, s32 sinQ16}/entry
nd  = NDIST;                                      // s32, PLAIN pixels 134..1122
dst = vga_page + 0x3e30;                          // row 199, col 0 (Mode X)
for (col = 0; col < 320; col++) {
    for (k = 0; k <= 69; k++) {                   // 70 ground rows, bottom-up
        u16 wx = ((ray[0] * nd[k]) >> 16) + camX;
        u16 wy = ((ray[1] * nd[k]) >> 16) + camY;
        u8 *tile = tilemap_ptr[(wy>>5)*128 + (wx>>5)];   // pointer-per-tile
        dst[-k*80] = tile[(wy & 31)*32 + (wx & 31)];
    }
    nd += 70; ray += 2;  if (ray > TRIG_LAST) ray = TRIG;  // 1 angle unit/col
    // Mode X plane cycling: plane<<=1, every 4 cols dst++
}
```

- FOV: 320 columns × 1/1920 circle = **60°**. One TRIG entry per column.
- **NDIST = 320 cols × 70 rows × s32, column-major, plain integer pixels**
  (134→1122). Fisheye correction is baked in per column. Detail levels 2/3
  render 160/80 rays 2px/4px wide, striding NDIST by 140/280 and TRIG by 2/4
  entries.
- **World is 4096×4096 px (128×128 tiles)**. The 64×64 track map sits at tile
  offset (32,32) → world +1024 px; everything outside is the border tile
  (tile 0). N.GAM coordinates get +0x400 added by the game.
- Tile storage: pointer map 128×128 of ptrs into a tileset arena:
  108 tiles × 1024 bytes (32×32 row-major). Tiles 0–53 from primary PCX pair,
  54–107 from secondary.
- **The "ma*.pcx" files are per-pixel surface MASKS**, same tile layout, stored
  at arena+0x1b000: each ground pixel selects surface word A or B of the
  tile's 8-byte .SIN record (`{u32 A, u32 B}` per tile type; the 12-byte file
  records drop their first 4 bytes on load). Surface code 3 = wall.

## Screen layout (320×200, Mode X, 80-byte row pitch)

Single player / P1: rows 0–109 static backdrop (back*.pcx, drawn once at race
start), rows **110–129 sky panorama strip**, rows **130–199 ground** (70 rows).
Horizon = row 130; eye row ≈ 120. Split-screen P2 (top): sky 10–29, ground
30–99. HUD overlays the backdrop area; sprites clamp top at row 110.

## Sky panorama (N.PAR)

N.PAR = **1280×20 pixel strip** (25600 bytes) = two copies of a 640-px
panorama. Converted to planar VGA offscreen at plane offset 0x7d00. Each frame
a 320-px window is latch-copied to the sky rows. Scroll: window byte offset
0..240 (4 px/byte); while steering it moves by a ramping velocity 1→4
(16 during spin), decaying when straight — velocity-driven, not a pure
function of angle. Wrap: 0→+160 bytes, 240→−160.

## Camera model

- Camera trails the kart: kart_pos = cam + dir(angle)·NDIST[col][0] (≈134 px).
  Turning pivots around the KART: cam += kart_old − kart_new.
- Wall whiskers: probe points cam + dir(angle±18)·124; surface code 3 ⇒
  auto-rotate ∓18/frame, push back 1 px, play sound 0x15.
- No camera height variable — height baked into NDIST and the 18000 constant.
- View culling triangle: {cam, cam ± dir(angle∓160)·1122}, bbox for sprites.

## Sprite projection (FUN_000255d4)

```
z = (cosθ·dx + sinθ·dy) >> 16          // depth
x = (cosθ·dy − sinθ·dx) >> 16          // lateral
w = (z * 0x93CD) >> 16                 // 0x93CD = tan(30°)·65536
col = 160 − (x·160)/w
dist = isqrt(x² + z²);  dist = (dist * VIEW[col]) >> 14   // fisheye, Q14
visible if 0x4f < dist < 0x463
scale bucket = clamp((dist − 124)/70, 0..9)   // 10 pre-scaled sizes
row = ROWTAB[dist]  where h = round(18000/dist) clamp [21,240]; row = 120+h−h/2
```

- SLP = tan(angle) Q16, 1920 entries; frustum wedge test against slopes at
  angle±160.

## VGA specifics

- Mode X unchained; pages at 0 and 16000; flip via CRTC 0xC/0xD + vsync wait.
- VGA→VGA blits in write mode 1 (latch), 4px/byte.
- Sprites stored transposed because blitters walk screen COLUMNS (one MapMask
  OUT per column instead of per pixel).
- Palette cycling: DAC entries 0xa1–0xa4+0x94 and 0xad/0xac, 0xae/0xaf,
  0xa8–0xab, periods from N.GAM lines 26–28.

## Open items

- TRIG.DAT raw byte interpretation vs dumped values (semantics = Q16 cos/sin
  per code; file bytes look <<16-shifted in spots — resolve at implementation
  by checking known identities, or synthesize bit-exact cos/sin·65536).
- VIEW file stores value<<16 of the Q14 factor actually consumed
  (hi word = 14186..16384 = cos(colangle)·16384).
- N.PAR panorama: confirm whether back*.pcx rows 110–129 under the strip ever
  show (they're overdrawn each frame).
