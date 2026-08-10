# OpenWacky — a Wacky Wheels engine reimplementation

Reverse engineering Apogee's **Wacky Wheels** (Beavis Soft, 1994) and rebuilding
its engine for modern platforms, using the original game data (which you must
own — it is not included).

## Status

- [x] `WACKY.DAT` archive extraction (`tools/wdat.py`)
- [x] PCX variant decoding incl. palette recovery (`tools/wpcx.py`)
- [x] Track map rendering — all 42 tracks (`tools/wtrack.py`)
- [x] Kart + object sprite ripping (`tools/wsp.py`, `SPRITE.ATR`)
- [x] Native macOS engine (C11 + SDL2, universal x86_64+arm64), reads
      WACKY.DAT directly (`src/`, CMake)
- [x] WW.EXE fully decompiled (Ghidra + LX loader) and analyzed —
      byte-exact algorithm specs in `docs/`
- [x] **Faithful renderer**: authentic column ray-caster (320 rays × 70 rows,
      NDIST/TRIG tables), N.PAR horizon panorama, back*.pcx backdrop,
      4096×4096 world with border tile
- [x] **Faithful physics**: 11.34 Hz tick, VEL.TAB velocity index model,
      per-pixel surface types (masks + N.SIN), walls + whisker scraping,
      hop/handbrake, turbo pads, SDX drag/grip
- [ ] Backdrop noise bands (verify vs original), player steer frames
- [ ] AI opponents (.RD waypoints), world objects (.SPW), laps/positions (.POS)
- [ ] HUD (minimap via MAP.XY, timer, speedo), items, game modes
- [ ] KLM (AdLib) music + VOC sound playback
- [ ] .app bundle + icon

## Building / running the engine

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release   # fetches+builds SDL2 (universal)
cmake --build build -j8
./build/openwacky ../WACKY.DAT [track#]
```

Controls: arrows drive, N/P switch track, C switch kart, R reset,
[ / ] adjust sprite angle offset (calibration aid), ESC quit.

See [FORMATS.md](FORMATS.md) for byte-level format documentation.

## Layout

```
tools/       Python format tools (extract/convert)
extracted/   raw files out of WACKY.DAT      (gitignore)
png/         converted full-screen graphics  (gitignore)
tracks/      rendered 2048x2048 track maps   (gitignore)
sprites/     ripped sprite sheets            (gitignore)
src/         (future) engine source
```

## Usage

```sh
python3 tools/wdat.py x ../WACKY.DAT extracted
.venv/bin/python tools/wpcx.py extracted/*.PCX png        # needs Pillow
PYTHONPATH=tools .venv/bin/python tools/wtrack.py extracted 1,2,3 tracks
PYTHONPATH=tools .venv/bin/python tools/wsp.py extracted/CARS.SP 38 28 extracted/A_F1.PCX sprites/cars.png
```
