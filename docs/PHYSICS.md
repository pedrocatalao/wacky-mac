# WW.EXE physics, input, surfaces — reverse engineering notes

From Ghidra decompilation analysis. Line refs → scratchpad ww_decomp.c.

## Timing

- PIT-driven counters: 136 Hz master (`0x12305E/freq` divisor), 10 Hz race
  clock, 1 Hz.
- Game loop is frame-locked with a floor: busy-wait until ≥ `DAT_0007ea5c`
  master ticks elapsed; default floor = 12 → **tick = 12/136 s ≈ 88.2 ms
  ≈ 11.34 Hz**. Cmdline can set 12/6/0 (0 = uncapped, vsync-paced).
- One physics tick per rendered frame. Demo playback forces floor 12
  (deterministic input streams = *.BMC, 3 bytes/frame).
- Faithful default: fixed 11.34 Hz simulation+render.

## Velocity model (per tick)

State: velocity index idx 0..200, speed = active_tab[idx] (u16, 0..80
world-units/tick). Active table: VEL.TAB = 12 HP engine, VEL2.TAB = 6 HP
(menu choice; kid mode forces 6 HP).

```
brake && idx>0 : idx -= 8 (min 0), skid sfx
!accel         : idx -= 4 (min 0)                       // coast
accel          : idx += 10 (net/modem: +6), cap 100     // kid mode: idx = 54
turbo          : idx = 160 frozen for 16 ticks          // turbo arrows (surface
                                                        // type 2, heading must be
                                                        // within per-track window)
speed = tab[idx]
surface drag   : speed -= (drag * speed) >> 16          // drag = SDX 16.16/type
```

## Steering (per tick)

- Turn rate by detail level: high 0x28=40, medium 0x24=36, low 0x20=32
  angle-units/tick (1920/circle). Net play forces 36. Independent of speed.
- Hop/handbrake: if moving, turn 120/tick for 5 ticks (≈112°).
- Spin-out: 8 ticks × 120 = 960 units = 180°; speed decays via drag; ends when
  speed < 1 (idx = 0).
- Drift: at idx ≥ 100, steering held ≥ grip-threshold ticks (SDX per surface)
  → drift state, steering at half rate, timer 120 decaying −10/tick.
  Grip threshold 0 = instant spin (ice).
- Pivot: turning keeps the point 124 units (whisker) / NDIST[0] (≈134,
  camera) ahead fixed — the rear swings, not the nose.

## Movement & collision (per tick)

- pos += dir(angle) · speed, walked in 1-unit Bresenham steps; each step
  probes the NOSE (pos + dir·124):
  - surface 3 (wall): stop, idx=0, push 32 units backward.
  - surface 10 (water/pit): sink state machine (depth +8/tick to 40, rise
    −8/tick), fixed slow speed, steering disabled.
  - object within Manhattan 14: hit; kart within 14 (18 in full check): bump.
- Whiskers at angle±18 × 124 units: wall → turn away 18 units, nudge 1
  forward, scrape sfx/anim.
- Surfaces 6 / 0xF: ramp pads — NO ballistic physics; sets "wings" latch,
  winged sprite anim (8 fly frames), ground physics continues; lands when
  off pad type.

## Surfaces

- Tile lookup is per-pixel: 128×128 pointer grid (track 64×64 at +32,+32 →
  world offset +0x400; playfield 0x400..0xBFF, probes clamp 0..0xFFF);
  mask plane (second PCX of each tileset pair) selects `.SIN` record word:
  type = mask ? typeB : typeA. .SIN = 108 × 12 bytes; first 4 bytes unused;
  {u32 typeA, u32 typeB}.
- WACKY.SDX: per-surface-type 302-byte records: +0x120 u32 drag (16.16),
  +0x124 u16 grip threshold, +0x126/+0x12A ramp anim pointers, rest sound.
- Type codes: 2 turbo, 3 wall, 5 soft (sprite sinks 16px), 6/0xF ramps,
  10 water/pit; others differ only via SDX drag/grip.

## Input

Config blob: 2 players × 6 controls {scancode, flag}: left, right, accel,
fire, brake, hop.

## AI

Same velocity table; top index by class/engine: Amateur 89/90, Pro 94/92,
Champion 99/94, TimeTrial/Kid 84; rubber-band bonus +16/+28/+36/+40 by
position. Waypoint steering via N.RD (not fully traced).

## Progress / laps

N.POS grid: `pos[(y-0x400)>>5][(x-0x400)>>5]`, increasing values along track;
wrong-way when decreasing.
