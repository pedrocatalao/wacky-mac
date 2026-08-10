/* World sprites: SPRITE.ATR objects (.SPW placements), other karts, and the
 * authentic projection + .INF scaled-blit pipeline (docs/SPRITES_HUD.md).
 * Transcribed behavior: FUN_000255d4 (projection), FUN_00037487 (scaled blit),
 * FUN_000274a4 (dist->row table), painter's-sort display list.
 */
#include "wacky.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TYPES   40
#define MAX_INST    512
#define MAX_INF     8
#define INF_LEVELS  10
#define TANQ16      0x93CD      /* tan(30 deg) Q16 */

typedef struct {                /* one .INF scale record */
    int      w, h;
    int32_t *cols;              /* w * (h+1) s32: start offset + per-row steps */
} InfRec;

typedef struct {
    int    nrec;
    InfRec rec[INF_LEVELS];
} Inf;

typedef struct {                /* SPRITE.ATR record */
    int  behavior, w, h, size_class, nframes;
    char name[22];
    const uint8_t *pixels;      /* frames, column-major */
    uint32_t bytes;
    int  inf;                   /* index into inf[] matched by dimensions */
} ObjType;

typedef struct {                /* .SPW instance */
    int type, anim, x, y, frame;
} ObjInst;

struct WScene {
    ObjType  types[MAX_TYPES];
    int      ntypes;
    ObjInst  inst[MAX_INST];
    int      ninst;
    Inf      inf[MAX_INF];
    int      ninf;
    int16_t  rowtab[0x1000];    /* dist -> screen ground row */
    const uint8_t *squash;      /* GENEF.SP + 0x1DD9: 4 frames of 38x28 */
    /* other karts (static grid until AI lands) */
    int      kart_x[8], kart_y[8], kart_angle[8];
};

/* ---- loading ---- */

static bool inf_load(Inf *f, const WDat *dat, const char *name) {
    uint32_t len;
    const uint8_t *d = wdat_find(dat, name, &len);
    if (!d || len < 8) return false;
    uint32_t off = 0;
    f->nrec = 0;
    while (f->nrec < INF_LEVELS && off + 4 <= len) {
        uint16_t w, h;
        memcpy(&w, d + off, 2);
        memcpy(&h, d + off + 2, 2);
        if (w == 0 || h == 0 || w > 320 || h > 200) break;
        size_t n = (size_t)w * (h + 1);
        if (off + 4 + n * 4 > len) break;
        InfRec *r = &f->rec[f->nrec];
        r->w = w;
        r->h = h;
        r->cols = malloc(n * 4);
        if (!r->cols) return false;
        memcpy(r->cols, d + off + 4, n * 4);
        off += 4 + (uint32_t)(n * 4);
        f->nrec++;
    }
    return f->nrec > 0;
}

static int inf_for_dims(WScene *s, const WDat *dat, int w, int h) {
    char name[24];
    for (int i = 0; i < s->ninf; i++)
        if (s->inf[i].nrec && s->inf[i].rec[0].w == w && s->inf[i].rec[0].h == h)
            return i;
    if (s->ninf >= MAX_INF) return -1;
    snprintf(name, sizeof name, "%dX%d.INF", w, h);
    if (!inf_load(&s->inf[s->ninf], dat, name)) return -1;
    return s->ninf++;
}

WScene *wscene_load(const WDat *dat, const WTrack *t, int tracknum) {
    WScene *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    uint32_t len;

    /* dist -> row table (FUN_000274a4): row = 120 + clamp(rnd(18000/d),21,240)/2 */
    for (int d = 2; d < 0x1000; d++) {
        int h = (18000 + d / 2) / d;
        if (h < 21) h = 21;
        if (h > 240) h = 240;
        s->rowtab[d] = (int16_t)(120 + h / 2);
    }
    s->rowtab[0] = s->rowtab[1] = s->rowtab[2];

    /* SPRITE.ATR: u16 header, then 40-byte records {9 x u16, char[22] name} */
    const uint8_t *atr = wdat_find(dat, "SPRITE.ATR", &len);
    if (atr) {
        int n = (int)((len - 2) / 40);
        if (n > MAX_TYPES) n = MAX_TYPES;
        for (int i = 0; i < n; i++) {
            const uint8_t *r = atr + 2 + (size_t)i * 40;
            uint16_t f[8];
            memcpy(f, r, 16);
            ObjType *o = &s->types[i];
            o->behavior = (int16_t)f[0];
            o->w = f[2];
            o->h = f[3];
            o->size_class = f[6];
            o->nframes = f[7];
            memcpy(o->name, r + 0x10, 20);
            o->name[20] = 0;
            o->pixels = wdat_find(dat, o->name, &o->bytes);
            o->inf = inf_for_dims(s, dat, o->w, o->h);
        }
        s->ntypes = n;
    }

    /* karts scale via 38X28.INF, projectiles via 18X13.INF */
    inf_for_dims(s, dat, 38, 28);
    inf_for_dims(s, dat, 18, 13);

    /* squashed-kart frames live inside GENEF.SP */
    {
        uint32_t glen;
        const uint8_t *g = wdat_find(dat, "GENEF.SP", &glen);
        if (g && glen > 0x1DD9 + 4 * 0x428) s->squash = g + 0x1DD9;
    }

    /* .SPW: u16 count, then 6 x s16 {type, anim, x, y, ?, frame}; +0x400 */
    char name[16];
    snprintf(name, sizeof name, "%d.SPW", tracknum);
    const uint8_t *spw = wdat_find(dat, name, &len);
    if (spw && len >= 2) {
        uint16_t cnt;
        memcpy(&cnt, spw, 2);
        if (cnt > MAX_INST) cnt = MAX_INST;
        for (int i = 0; i < cnt; i++) {
            int16_t f[6];
            memcpy(f, spw + 2 + (size_t)i * 12, 12);
            ObjInst *in = &s->inst[s->ninst];
            in->type = f[0];
            in->anim = 0;
            in->x = f[2] + WW_MAP_OFF;
            in->y = f[3] + WW_MAP_OFF;
            in->frame = 0;
            if (in->type >= 0 && in->type < s->ntypes &&
                s->types[in->type].pixels)
                s->ninst++;
        }
    }

    /* start grid (FUN_00016f90): one row behind slot 0 */
    static const int order[8] = { 4, 3, 2, 1, 0, 7, 6, 5 };
    s->kart_x[0] = t->start_x[0];
    s->kart_y[0] = t->start_y[0];
    for (int k = 1; k < 8; k++) {
        s->kart_x[order[k - 1] == 0 ? k : k] = 0; /* placeholder, set below */
    }
    for (int k = 1; k < 8; k++) {
        s->kart_x[k] = t->start_x[0] + 0x50 - 0x14 * k;
        s->kart_y[k] = t->start_y[0];
    }
    for (int k = 0; k < 8; k++) s->kart_angle[k] = 0x1E0;
    return s;
}

void wscene_free(WScene *s) {
    if (!s) return;
    for (int i = 0; i < s->ninf; i++)
        for (int r = 0; r < s->inf[i].nrec; r++)
            free(s->inf[i].rec[r].cols);
    free(s);
}

/* collision probe: any live object within Manhattan 0xE of (x,y). Marks it
 * touched (state 1); only SOLID scenery (behavior <= 0) blocks movement —
 * pickups are resolved after the tick by wscene_resolve_pickups. */
int wscene_hit_object(void *ctx, int x, int y) {
    WScene *s = ctx;
    if (!s) return 0;
    for (int i = 0; i < s->ninst; i++) {
        ObjInst *in = &s->inst[i];
        if (in->anim == -1) continue;
        int dx = in->x - x, dy = in->y - y;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        if (dx + dy < 0xE) {
            in->anim = 1;
            return s->types[in->type].behavior <= 0;
        }
    }
    return 0;
}

/* read-only probe used by projectiles: any live object within Manhattan 0xE.
 * Unlike the kart probe this does NOT mark the object as touched, so a
 * projectile flying past a crate cannot collect it (ProjProbe @13022). */
int wscene_blocks(void *ctx, int x, int y) {
    const WScene *s = ctx;
    if (!s) return 0;
    for (int i = 0; i < s->ninst; i++) {
        const ObjInst *in = &s->inst[i];
        if (in->anim == -1) continue;
        int dx = in->x - x, dy = in->y - y;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        if (dx + dy < 0xE) return 1;
    }
    return 0;
}

/* SPRITE.ATR behavior: <=0 solid, 1 trigger, 2 ammo crate (+4, cap 99),
 * 3..8 weapon pickup (ignored while a weapon is already held).
 * Taken objects stay hidden until the race restarts. */
void wscene_resolve_pickups(WScene *s, WPhys *p) {
    if (!s) return;
    for (int i = 0; i < s->ninst; i++) {
        ObjInst *in = &s->inst[i];
        if (in->anim != 1) continue;
        int behavior = s->types[in->type].behavior;
        if (behavior == 1) {
            in->anim = -1;
        } else if (behavior >= 2 && p->ammo < 99) {
            if (p->weapon_id == 0 || behavior == 2) {
                in->anim = -1;
                if (behavior == 2) {
                    p->ammo += 4;
                    if (p->ammo > 99) p->ammo = 99;
                } else {
                    p->weapon_id = behavior;
                }
            }
        }
    }
}

/* ---- per-tick object animation (FUN_000261fc) ---- */
void wscene_tick(WScene *s) {
    for (int i = 0; i < s->ninst; i++) {
        ObjInst *in = &s->inst[i];
        if (in->anim == -1) continue;
        const ObjType *ty = &s->types[in->type];
        if (ty->nframes > 1 && ++in->frame >= ty->nframes) in->frame = 0;
    }
}

/* ---- projection + display list ---- */

typedef struct {
    int32_t dist;
    int     sx;          /* screen center column */
    int     bucket;
    const uint8_t *frame;
    const Inf *inf;
    int     src_w, src_h;
} DrawEnt;

static uint32_t isqrt32(uint32_t v) {
    uint32_t r = 0, b = 1u << 30;
    while (b > v) b >>= 2;
    while (b) {
        if (v >= r + b) { v -= r + b; r = (r >> 1) + b; }
        else r >>= 1;
        b >>= 2;
    }
    return r;
}

/* project one world sprite; returns false if culled.
 * bucket_base is 0x7C for karts/objects, 0x3E for projectiles (they render
 * one size step larger at the same distance). */
static bool project_b(const WTables *tb, const WPhys *p,
                      int wx, int wy, DrawEnt *e, int bucket_base) {
    int32_t dx = wx - p->posx, dy = wy - p->posy;
    int32_t cq = tb->cosq[p->angle], sq = tb->sinq[p->angle];
    int32_t z = (cq * dx + sq * dy) >> 16;
    if (z <= 0) return false;
    int32_t lx = (dy * cq - dx * sq) >> 16;
    int32_t half = (z * TANQ16) >> 16;
    if (half <= 0) return false;
    int sx = 0xA0 + (int)((lx * 0xA0) / half);
    if (sx < 0 || sx > 0x13F) return false;
    uint32_t dist = isqrt32((uint32_t)(lx * lx) + (uint32_t)(z * z));
    int64_t d2 = (int64_t)dist * tb->viewq14[sx];
    dist = (uint32_t)(d2 >> 14);
    if ((d2 & 0x3FFF) > 0x1F9F) dist++;
    if (dist < 0x50 || dist > 0x462) return false;
    int bucket = ((int)dist - bucket_base) / 0x46;
    if (bucket < 0) bucket = 0;
    if (bucket > 9) bucket = 9;
    e->dist = (int32_t)dist;
    e->sx = sx;
    e->bucket = bucket;
    return true;
}

static bool project(const WTables *tb, const WPhys *p,
                    int wx, int wy, DrawEnt *e) {
    return project_b(tb, p, wx, wy, e, 0x7C);
}

static void draw_scaled(uint32_t *fb, const uint8_t dac[768], const DrawEnt *e,
                        const int16_t *rowtab) {
    const InfRec *r = &e->inf->rec[e->bucket < e->inf->nrec ? e->bucket
                                                           : e->inf->nrec - 1];
    int base_row = rowtab[e->dist & 0xFFF];
    int y0 = base_row - r->h;
    int x0 = e->sx - r->w / 2;
    for (int c = 0; c < r->w; c++) {
        int sx = x0 + c;
        if (sx < 0 || sx >= WW_SCREEN_W) continue;
        const int32_t *col = r->cols + (size_t)c * (r->h + 1);
        int32_t off = col[0];
        for (int y = 0; y < r->h; y++) {
            int sy = y0 + y;
            if (sy >= WW_SKY_Y0 && sy < WW_SCREEN_H &&
                off >= 0 && (uint32_t)off < (uint32_t)(e->src_w * e->src_h)) {
                uint8_t px = e->frame[off];
                if (px != WW_SP_TRANSPARENT)
                    fb[(size_t)sy * WW_SCREEN_W + sx] =
                        0xFF000000u | (uint32_t)dac[px * 3 + 2] << 16 |
                        (uint32_t)dac[px * 3 + 1] << 8 | dac[px * 3];
            }
            off += col[1 + y];
        }
    }
}

static int cmp_far_first(const void *a, const void *b) {
    return ((const DrawEnt *)b)->dist - ((const DrawEnt *)a)->dist;
}

void wscene_draw(uint32_t *fb, WScene *s, const WTrack *t, const WTables *tb,
                 const WPhys *p, const uint8_t *cars_px, int player_kart,
                 const WAi *ai, const WWeapons *weap) {
    DrawEnt list[MAX_INST + 8];
    int n = 0;

    const Inf *kart_inf = NULL;
    for (int i = 0; i < s->ninf; i++)
        if (s->inf[i].nrec && s->inf[i].rec[0].w == 38 && s->inf[i].rec[0].h == 28)
            kart_inf = &s->inf[i];

    /* objects */
    for (int i = 0; i < s->ninst && n < MAX_INST; i++) {
        ObjInst *in = &s->inst[i];
        if (in->anim == -1) continue;
        const ObjType *ty = &s->types[in->type];
        if (!ty->pixels || ty->inf < 0) continue;
        DrawEnt e;
        if (!project(tb, p, in->x, in->y, &e)) continue;
        e.inf = &s->inf[ty->inf];
        e.src_w = ty->w;
        e.src_h = ty->h;
        uint32_t fbytes = (uint32_t)(ty->w * ty->h);
        uint32_t fi = (uint32_t)(in->frame % (ty->nframes ? ty->nframes : 1));
        if ((fi + 1) * fbytes <= ty->bytes) {
            e.frame = ty->pixels + fi * fbytes;
            list[n++] = e;
        }
    }

    /* other karts: display octant from .RD compass (pre-remapped in file);
     * frame = (compass - camOct) & 7 (viewTable with K=0, FUN_000287ec) */
    if (cars_px && kart_inf && ai) {
        int cam_oct = p->angle / 240;
        if (p->angle % 240 > 0x77) cam_oct++;
        cam_oct &= 7;
        for (int k = 1; k < 8; k++) {
            int kx, ky, compass, hitframe = 0;
            int mode = wai_kart_render(ai, k, &kx, &ky, &compass, &hitframe);
            if (mode == 3) continue;                 /* destroyed and gone */
            DrawEnt e;
            if (!project(tb, p, kx, ky, &e)) continue;
            e.inf = kart_inf;
            e.src_w = 38;
            e.src_h = 28;
            int sprite = k == player_kart ? 0 : k;   /* keep player's kart unique */
            if (mode == 2) {                         /* squashed flat */
                if (!s->squash) continue;
                e.frame = s->squash + (size_t)hitframe * 0x428;
            } else {
                /* spinning karts cycle their own 8 rotation frames;
                 * viewTable seed K=6 gives the rear view for same-heading */
                int frame = (mode == 1) ? hitframe : ((compass - cam_oct + 6) & 7);
                e.frame = cars_px + ((size_t)sprite * 12 + frame) * 38 * 28;
            }
            list[n++] = e;
        }
    }

    /* projectiles: 18x13 frames, scale bucket base 0x3E (FUN_0002632c) */
    if (weap) {
        const Inf *pinf = NULL;
        for (int i = 0; i < s->ninf; i++)
            if (s->inf[i].nrec && s->inf[i].rec[0].w == 18 && s->inf[i].rec[0].h == 13)
                pinf = &s->inf[i];
        int total = wweap_count();
        for (int i = 0; i < total && n < MAX_INST + 8; i++) {
            int px, py;
            const uint8_t *sprite;
            if (!wweap_enum(weap, i, &px, &py, &sprite)) continue;
            DrawEnt e;
            if (!project_b(tb, p, px, py, &e, 0x3E)) continue;
            if (!pinf) continue;
            e.inf = pinf;
            e.src_w = 18;
            e.src_h = 13;
            e.frame = sprite;
            list[n++] = e;
        }
    }

    qsort(list, (size_t)n, sizeof *list, cmp_far_first);
    for (int i = 0; i < n; i++)
        draw_scaled(fb, t->dac, &list[i], s->rowtab);
}
