#include "wacky.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t pal_rgba(const uint8_t pal[768], uint8_t idx) {
    return 0xFF000000u | (uint32_t)pal[idx * 3 + 2] << 16 |
           (uint32_t)pal[idx * 3 + 1] << 8 | pal[idx * 3];
}

/* .GAM: plain CRLF text lines (see FORMATS.md) */
typedef struct {
    char lines[28][32];
    int  n;
} Gam;

static bool gam_parse(const uint8_t *buf, uint32_t len, Gam *g) {
    g->n = 0;
    uint32_t i = 0;
    while (i < len && g->n < 28) {
        uint32_t start = i;
        while (i < len && buf[i] != '\r') i++;
        uint32_t l = i - start;
        if (l >= sizeof g->lines[0]) l = sizeof g->lines[0] - 1;
        memcpy(g->lines[g->n], buf + start, l);
        g->lines[g->n][l] = 0;
        while (l > 0 && g->lines[g->n][l - 1] == ' ') g->lines[g->n][--l] = 0;
        g->n++;
        i += 2;
    }
    return g->n >= 25;
}

/* slice a 320x200 tileset image into 54 tiles of 32x32 (10 per row) */
static void slice_tiles(const WImage *img, uint8_t out[][WW_TILE * WW_TILE], int count) {
    for (int j = 0; j < count; j++) {
        int sx = (j % 10) * WW_TILE, sy = (j / 10) * WW_TILE;
        for (int y = 0; y < WW_TILE; y++) {
            if (sy + y >= img->h) {
                memset(out[j] + y * WW_TILE, 0, WW_TILE);
                continue;
            }
            memcpy(out[j] + y * WW_TILE,
                   img->pixels + (size_t)(sy + y) * img->w + sx, WW_TILE);
        }
    }
}

static bool load_pcx(const WDat *dat, const char *name, WImage *img) {
    uint32_t len;
    const uint8_t *p = wdat_find(dat, name, &len);
    return p && wpcx_decode(p, len, img);
}

bool wtrack_load(WTrack *t, const WDat *dat, int tracknum) {
    memset(t, 0, sizeof *t);
    char name[40];
    uint32_t len;

    snprintf(name, sizeof name, "%d.GAM", tracknum);
    const uint8_t *gambuf = wdat_find(dat, name, &len);
    Gam gam;
    if (!gambuf || !gam_parse(gambuf, len, &gam)) return false;

    /* tileset pixel PCXs (lines 1-2) and their mask PCXs (lines 3-4);
     * DAT names carry an "a_" prefix */
    WImage img;
    for (int i = 0; i < 2; i++) {
        snprintf(name, sizeof name, "a_%s", gam.lines[i]);
        if (!load_pcx(dat, name, &img)) return false;
        if (i == 0) memcpy(t->pal, img.pal, 768); /* race palette */
        slice_tiles(&img, &t->tiles[i * 54], 54);
        wimage_free(&img);

        snprintf(name, sizeof name, "a_%s", gam.lines[2 + i]);
        if (load_pcx(dat, name, &img)) {   /* masks (some tracks lack one) */
            slice_tiles(&img, &t->masks[i * 54], 54);
            wimage_free(&img);
        }
    }

    /* backdrop (line 5): keep indices; the game displays it through the race
     * palette (from the primary tileset), not the backdrop PCX's own */
    if (load_pcx(dat, gam.lines[4], &img)) {
        size_t n = (size_t)WW_SCREEN_W * WW_SCREEN_H;
        t->back = malloc(n);
        if (t->back) {
            memset(t->back, 0, n);
            size_t have = (size_t)img.w * img.h;
            memcpy(t->back, img.pixels, have < n ? have : n);
        }
        wimage_free(&img);
    }

    /* start positions: game adds +0x400 world offset to N.GAM coords */
    for (int i = 0; i < 8; i++) {
        t->start_x[i] = atoi(gam.lines[8 + i * 2]) + WW_MAP_OFF;
        t->start_y[i] = atoi(gam.lines[9 + i * 2]) + WW_MAP_OFF;
    }

    /* color cycling config (lines 25-28) + initial phase-0 DAC state */
    memcpy(t->dac, t->pal, 768);
    t->cycle_on = gam.n > 24 && atoi(gam.lines[24]) != 0;
    t->cycle_per_a  = gam.n > 25 ? atoi(gam.lines[25]) : 0;
    t->cycle_hold_a = gam.n > 26 ? atoi(gam.lines[26]) : 0;
    t->cycle_per_b  = gam.n > 27 ? atoi(gam.lines[27]) : 0;
    /* rest state = file palette; the game only writes cycle colors once the
     * first period elapses (sourced from the BEACH.PCX palette) */

    /* tile map: 64x64 placed at grid (32,32); border tile 0 elsewhere */
    snprintf(name, sizeof name, "%d.M", tracknum);
    const uint8_t *m = wdat_find(dat, name, &len);
    if (!m || len < 64 * 64) goto fail;
    for (int ty = 0; ty < 64; ty++)
        for (int tx = 0; tx < 64; tx++) {
            uint8_t idx = m[ty * 64 + tx];
            t->grid[(ty + 32) * WW_GRID + tx + 32] =
                idx < WW_NTILES ? idx : 0;
        }

    /* N.SIN: 108 x 12-byte records; surface type codes at u16 offsets 4 and 8
     * (verified: code 3 = wall appears on barrier tiles) */
    snprintf(name, sizeof name, "%d.SIN", tracknum);
    const uint8_t *sinb = wdat_find(dat, name, &len);
    if (sinb) {
        int n = (int)(len / 12);
        if (n > WW_NTILES) n = WW_NTILES;
        for (int i = 0; i < n; i++) {
            uint16_t a, b;
            memcpy(&a, sinb + i * 12 + 4, 2);
            memcpy(&b, sinb + i * 12 + 8, 2);
            t->sin_a[i] = a;
            t->sin_b[i] = b;
        }
    }

    /* N.POS: 64x64 u16 progress grid (lap/wrong-way tracking) */
    snprintf(name, sizeof name, "%d.POS", tracknum);
    const uint8_t *posb = wdat_find(dat, name, &len);
    if (posb && len >= sizeof t->pos) {
        memcpy(t->pos, posb, sizeof t->pos);
        for (int i = 0; i < 64 * 64; i++)
            if (t->pos[i] > t->pos_max) t->pos_max = t->pos[i];
    }

    /* N.PAR: 1280x20 horizon panorama, raw indices */
    snprintf(name, sizeof name, "%d.PAR", tracknum);
    const uint8_t *par = wdat_find(dat, name, &len);
    if (par && len >= sizeof t->par) memcpy(t->par, par, sizeof t->par);

    return true;
fail:
    wtrack_free(t);
    return false;
}

void wtrack_free(WTrack *t) {
    free(t->back);
    memset(t, 0, sizeof *t);
}

/* group A (FUN_00037c43): DAC 161-164 <- 4 triples at beach+phase*15,
 * DAC 148 <- triple 4 */
void ww_palette_cycle_a(WTrack *t, const uint8_t beach[768], int phase_a) {
    const uint8_t *src = beach + (phase_a % 9) * 15;
    memcpy(t->dac + 161 * 3, src, 12);
    memcpy(t->dac + 148 * 3, src + 12, 3);
}

/* group B (FUN_00037c84): base = beach entry 45 + phase*4 triples;
 * DAC 173,172,174,175 <- triples 0-3, DAC 168-171 <- 4 triples at +0x30 */
void ww_palette_cycle_b(WTrack *t, const uint8_t beach[768], int phase_b) {
    const uint8_t *b = beach + 45 * 3 + (phase_b % 4) * 12;
    memcpy(t->dac + 173 * 3, b, 3);
    memcpy(t->dac + 172 * 3, b + 3, 3);
    memcpy(t->dac + 174 * 3, b + 6, 3);
    memcpy(t->dac + 175 * 3, b + 9, 3);
    memcpy(t->dac + 168 * 3, b + 0x30, 12);
}

/* ---- engine tables ---- */

static const uint8_t *find_read(const WDat *dat, const char *name, uint32_t need,
                                uint32_t *len_out) {
    uint32_t len;
    const uint8_t *p = wdat_find(dat, name, &len);
    if (!p || len < need) return NULL;
    if (len_out) *len_out = len;
    return p;
}

bool wtables_load(WTables *tb, const WDat *dat) {
    memset(tb, 0, sizeof *tb);

    /* TRIG.DAT: 1920 x {s32 cos, s32 sin}, Q16 (the game's own table,
     * including its rounding quirks — loaded verbatim for fidelity) */
    const uint8_t *p = find_read(dat, "TRIG.DAT", WW_ANGLES * 8, NULL);
    if (!p) return false;
    for (int k = 0; k < WW_ANGLES; k++) {
        memcpy(&tb->cosq[k], p + k * 8, 4);
        memcpy(&tb->sinq[k], p + k * 8 + 4, 4);
    }

    /* NDIST: 320 cols x 70 rows of s32 distances (plain pixels, 133..1122) */
    p = find_read(dat, "NDIST", WW_SCREEN_W * WW_GROUND_ROWS * 4, NULL);
    if (!p) return false;
    memcpy(tb->ndist, p, WW_SCREEN_W * WW_GROUND_ROWS * 4);

    /* VIEW: 320 x s32 per-column fisheye factor, Q14 (center 16384) */
    p = find_read(dat, "VIEW", WW_SCREEN_W * 4, NULL);
    if (!p) return false;
    memcpy(tb->viewq14, p, WW_SCREEN_W * 4);

    p = find_read(dat, "VEL.TAB", 400, NULL);
    if (!p) return false;
    memcpy(tb->vel, p, 400);
    p = find_read(dat, "VEL2.TAB", 400, NULL);
    if (p) memcpy(tb->vel2, p, 400);

    /* BEACH.PCX palette = color-cycle source triples (loaded once at startup
     * by the original, FUN main init) */
    {
        uint32_t blen;
        const uint8_t *bp = wdat_find(dat, "BEACH.PCX", &blen);
        WImage bi;
        if (bp && wpcx_decode(bp, blen, &bi)) {
            memcpy(tb->beach_pal, bi.pal, 768);
            wimage_free(&bi);
        }
    }

    /* WACKY.SDX: u16 count, then 302-byte records indexed by surface type;
     * +0x120 u32 drag (16.16), +0x124 u16 grip threshold */
    uint32_t len;
    p = find_read(dat, "WACKY.SDX", 2, &len);
    if (p) {
        uint16_t count;
        memcpy(&count, p, 2);
        if (count > 64) count = 64;
        tb->sdx_count = count;
        for (int i = 0; i < count; i++) {
            const uint8_t *rec = p + 2 + (size_t)i * 0x12E;
            if ((size_t)(rec - p) + 0x12E > len) break;
            memcpy(&tb->sdx_drag[i], rec + 0x120, 4);
            memcpy(&tb->sdx_grip[i], rec + 0x124, 2);
            memcpy(&tb->sdx_eff_off[i], rec + 0x104, 4);
            memcpy(&tb->sdx_eff_size[i], rec + 0x114, 2);
            memcpy(&tb->sdx_eff_w[i], rec + 0x116, 2);
            memcpy(&tb->sdx_eff_h[i], rec + 0x118, 2);
            memcpy(&tb->sdx_eff_n[i], rec + 0x11E, 2);
        }
    }
    tb->effects = wdat_find(dat, "EFFECTS.SP", &tb->effects_len);
    return true;
}

bool wsprite_load(WSprite *s, const WDat *dat, const char *name,
                  int w, int h, const uint8_t pal[768]) {
    memset(s, 0, sizeof *s);
    uint32_t len;
    const uint8_t *buf = wdat_find(dat, name, &len);
    if (!buf || w <= 0 || h <= 0) return false;
    int frame_bytes = w * h;
    s->w = w;
    s->h = h;
    s->nframes = (int)(len / (uint32_t)frame_bytes);
    if (s->nframes == 0) return false;
    s->rgba = malloc((size_t)s->nframes * frame_bytes * 4);
    if (!s->rgba) return false;
    for (int f = 0; f < s->nframes; f++) {
        const uint8_t *fr = buf + (size_t)f * frame_bytes;
        uint32_t *dst = s->rgba + (size_t)f * frame_bytes;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                uint8_t idx = fr[x * h + y];   /* column-major source */
                dst[y * w + x] = idx == WW_SP_TRANSPARENT ? 0 : pal_rgba(pal, idx);
            }
    }
    return true;
}

void wsprite_free(WSprite *s) {
    free(s->rgba);
    memset(s, 0, sizeof *s);
}
