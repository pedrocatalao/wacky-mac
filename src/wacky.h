/* OpenWacky — native reimplementation of Wacky Wheels (Apogee/Beavis Soft 1994)
 * Faithful port: algorithms and constants from WW.EXE disassembly.
 * See ../FORMATS.md and ../docs/{RENDERER,PHYSICS,SPRITES_HUD}.md
 */
#ifndef WACKY_H
#define WACKY_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ---- WACKY.DAT archive ---- */

typedef struct {
    char     name[15];
    uint32_t size;
    uint32_t offset;
} WDatEntry;

typedef struct {
    uint8_t   *data;
    size_t     size;
    WDatEntry *entries;
    int        count;
} WDat;

bool           wdat_open(WDat *dat, const char *path);
void           wdat_close(WDat *dat);
const uint8_t *wdat_find(const WDat *dat, const char *name, uint32_t *size_out);

/* ---- PCX (WW variant) ---- */

typedef struct {
    int      w, h;
    uint8_t *pixels;
    uint8_t  pal[768];
} WImage;

bool wpcx_decode(const uint8_t *buf, size_t len, WImage *img);
void wimage_free(WImage *img);

/* ---- Authentic world model (from WW.EXE) ----
 * 128x128 tile grid of 32x32 tiles = 4096x4096 world; the 64x64 track map
 * occupies tiles (32,32)..(95,95) => +1024px offset; outside = border tile 0.
 * 108 tiles: 0-53 primary tileset PCX, 54-107 secondary. Each tileset PCX has
 * a paired mask PCX (per-pixel surface selector). Surface type =
 * mask ? sin[tile].typeB : sin[tile].typeA.
 */
#define WW_TILE       32
#define WW_GRID       128
#define WW_WORLD      4096
#define WW_MAP_OFF    0x400
#define WW_NTILES     108
#define WW_ANGLES     1920       /* angle units per full circle */
#define WW_SCREEN_W   320
#define WW_SCREEN_H   200
#define WW_GROUND_Y0  130        /* ground rows 130..199 (70 rows) */
#define WW_GROUND_ROWS 70
#define WW_SKY_Y0     110        /* N.PAR sky strip rows 110..129 */
#define WW_SKY_ROWS   20
#define WW_PAR_W      1280

/* surface type codes */
#define WW_SURF_TURBO 2
#define WW_SURF_WALL  3
#define WW_SURF_SOFT  5
#define WW_SURF_RAMP1 6
#define WW_SURF_WATER 10
#define WW_SURF_RAMP2 0xF

typedef struct {
    uint8_t  tiles[WW_NTILES][WW_TILE * WW_TILE];  /* pixel indices */
    uint8_t  masks[WW_NTILES][WW_TILE * WW_TILE];  /* surface mask plane */
    uint8_t  grid[WW_GRID * WW_GRID];              /* tile index per cell */
    uint32_t sin_a[WW_NTILES], sin_b[WW_NTILES];   /* N.SIN surface types */
    uint8_t  pal[768];                             /* pristine file palette (cycle source) */
    uint8_t  dac[768];                             /* working palette (what's displayed) */
    uint8_t  par[WW_PAR_W * WW_SKY_ROWS];          /* horizon panorama indices */
    uint8_t  *back;                                /* 320x200 backdrop indices
                                                      (displayed via race palette) */
    int      start_x[8], start_y[8];               /* world coords (+0x400 applied) */
    bool     cycle_on;                             /* N.GAM line 25 */
    int      cycle_per_a, cycle_hold_a, cycle_per_b; /* N.GAM lines 26-28 */
    uint16_t pos[64 * 64];                         /* N.POS progress grid */
    uint16_t pos_max;
} WTrack;

/* track progress at world position (WW.EXE FUN_0001260c) */
static inline uint16_t ww_progress_at(const WTrack *t, int x, int y) {
    x = (x - WW_MAP_OFF) >> 5;
    y = (y - WW_MAP_OFF) >> 5;
    if (x < 0 || x > 63 || y < 0 || y > 63) return 0;
    return t->pos[y * 64 + x];
}

/* palette color cycling (WW.EXE FUN_00028668/FUN_00037c43/FUN_00037c84):
 * rewrites DAC entries 161-164+148 (phase_a 0..8, with a hold at phase 3) and
 * 168-175 (phase_b 0..3) from source triples in the BEACH.PCX palette */
void ww_palette_cycle_a(WTrack *t, const uint8_t beach[768], int phase_a);
void ww_palette_cycle_b(WTrack *t, const uint8_t beach[768], int phase_b);

bool wtrack_load(WTrack *t, const WDat *dat, int tracknum);
void wtrack_free(WTrack *t);

static inline uint8_t ww_tile_at(const WTrack *t, uint16_t x, uint16_t y) {
    if (x > 0xFFF) x = 0xFFF;
    if (y > 0xFFF) y = 0xFFF;
    return t->grid[(y >> 5) * WW_GRID + (x >> 5)];
}

static inline uint32_t ww_surface_at(const WTrack *t, uint16_t x, uint16_t y) {
    if (x > 0xFFF) x = 0xFFF;
    if (y > 0xFFF) y = 0xFFF;
    uint8_t ti = t->grid[(y >> 5) * WW_GRID + (x >> 5)];
    uint8_t m = t->masks[ti][(y & 31) * WW_TILE + (x & 31)];
    return m ? t->sin_b[ti] : t->sin_a[ti];
}

/* ---- Engine tables ---- */

typedef struct {
    int32_t  cosq[WW_ANGLES], sinq[WW_ANGLES];  /* Q16 (synthesized; see docs) */
    int32_t  ndist[WW_SCREEN_W * WW_GROUND_ROWS]; /* column-major, plain px */
    int32_t  viewq14[WW_SCREEN_W];              /* per-column fisheye, Q14 */
    uint16_t vel[200], vel2[200];               /* 12 HP / 6 HP speed curves */
    uint8_t  beach_pal[768];                    /* BEACH.PCX palette: color-cycle
                                                   source triples (entries 0..64) */
    uint32_t sdx_drag[64];                      /* per surface type, 16.16 */
    uint16_t sdx_grip[64];
    /* per-surface engine effect, two layers (A big, B small); byte offsets
     * into EFFECTS.SP + frame geometry (WACKY.SDX record fields) */
    uint32_t sdx_effA_off[64], sdx_effB_off[64];
    uint16_t sdx_effA_size[64], sdx_effA_w[64], sdx_effA_h[64], sdx_effA_n[64];
    uint16_t sdx_effB_size[64], sdx_effB_w[64], sdx_effB_h[64], sdx_effB_n[64];
    const uint8_t *effects;                     /* EFFECTS.SP raw */
    uint32_t effects_len;
    int      sdx_count;
} WTables;

bool wtables_load(WTables *tb, const WDat *dat);

/* ---- Player physics (transcribed from WW.EXE; see src/physics.c) ---- */

typedef struct {
    int32_t posx, posy;      /* physics position (pivot; 0x7C behind sprite) */
    int32_t worldx, worldy;  /* kart sprite world position                    */
    int     angle;           /* 0..1919; race start = 0x1E0                   */
    int16_t speed;
    int     throttle;        /* velocity index 0..100 (0xA0 during turbo)     */
    int     steer_l, steer_r;
    int     hold_l, hold_r;  /* consecutive steer ticks                        */
    int     drift;           /* 0 / 1 left / 2 right                          */
    int     drift_timer;
    int     spin_dir, spin_step;
    int     hop_turn_dir, hop_turn_cnt;   /* handbrake sharp turn             */
    int     hop_state, hop_air, hop_height, hop_maxh;  /* ramp jumps          */
    int     turbo, turbo_timer;
    int     engine_state, engine_anim;   /* kart +0x62 / +0x54: exhaust anim */
    int     weapon_id;       /* 0 default hedgehog, 3..8 special (see docs)  */
    int     ammo;            /* hedgehog count, cap 99                        */
    int     fire_latch, last_fire_tick;
    int     in_water, splash;
    int     collide;         /* 0 none, 1 wall, 2 object, 3 kart, 4 ramp-hop  */
    int     collide_kart;    /* index of the kart hit when collide == 3       */
    int     object_hit;
    int     skid, scraping;
    uint32_t surface;
    uint32_t drag;           /* 16.16, from SDX */
    uint16_t grip;
} WPhys;

typedef struct {
    bool accel, brake, left, right, hop;
} WPhysInput;

/* collision probes into the world (objects / other karts), Manhattan < 0xE */
typedef struct {
    int (*hit_object)(void *ctx, int x, int y);
    int (*hit_kart)(void *ctx, int x, int y);
    void *obj_ctx, *kart_ctx;
} WCollide;

void wphys_reset(WPhys *p, const WTrack *t);
void wphys_tick(WPhys *p, const WTrack *t, const WTables *tb,
                const WPhysInput *in, int detail_level, const WCollide *col);

/* ---- AI karts (.RD position-scripted; see src/ai.c) ---- */

typedef struct WAi WAi;
WAi *wai_load(const WDat *dat, const WTrack *t, int tracknum);
void wai_free(WAi *ai);
void wai_reset(WAi *ai, const WTrack *t, int class_id, int engine12,
               const WTables *tb);
void wai_tick(WAi *ai, const WTables *tb, int32_t player_progress, int player_rank);
void wai_progress(WAi *ai, const WTrack *t);
void wai_kart_state(const WAi *ai, int i, int *x, int *y, int *compass);
int  wai_hit_kart(void *ctx, int x, int y);
/* projectile probes: kart within Manhattan 0x12, and the hit reaction
 * (spin_state 1 = spin 0x21 ticks, 2 = squash 0x32 ticks) */
int  wai_kart_at(void *ctx, int x, int y);
void wai_kart_hit(void *ctx, int idx, int spin_state, int tick);
void wai_kart_ram(WAi *ai, int idx);
int  wai_kart_render(const WAi *ai, int i, int *x, int *y, int *compass, int *frame);

/* ---- Weapons / projectiles (src/weapons.c) ---- */

typedef struct WWeapons WWeapons;
WWeapons *wweap_create(const WDat *dat);
void wweap_free(WWeapons *w);
void wweap_reset(WWeapons *w);
void wweap_fire(WWeapons *w, WPhys *p, const WTrack *t, const WTables *tb,
                bool fire_btn, int tick, int *sound_out);
void wweap_tick(WWeapons *w, const WTrack *t, const WTables *tb,
                const WCollide *col, int shooter_angle, int tick);
int  wweap_enum(const WWeapons *w, int i, int *x, int *y, const uint8_t **sprite);
int  wweap_count(void);

/* ---- World scene: objects (.SPW/SPRITE.ATR) + kart billboards ---- */

typedef struct WScene WScene;
WScene *wscene_load(const WDat *dat, const WTrack *t, int tracknum);
void    wscene_free(WScene *s);
void    wscene_tick(WScene *s);
int     wscene_hit_object(void *ctx, int x, int y);
int     wscene_blocks(void *ctx, int x, int y);
void    wscene_resolve_pickups(WScene *s, WPhys *p);
void    wscene_draw(uint32_t *fb, WScene *s, const WTrack *t, const WTables *tb,
                    const WPhys *p, const uint8_t *cars_px, int player_kart,
                    const WAi *ai, const WWeapons *weap);

/* ---- Sprites (.SP raw transposed frames) ---- */

#define WW_SP_TRANSPARENT 0   /* blitter color key (WW.EXE FUN_00036fd0) */

typedef struct {
    int       w, h, nframes;
    uint32_t *rgba;
} WSprite;

bool wsprite_load(WSprite *s, const WDat *dat, const char *name,
                  int w, int h, const uint8_t pal[768]);
void wsprite_free(WSprite *s);

#endif
