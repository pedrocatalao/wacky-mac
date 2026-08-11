/* In-race HUD — minimap, speedometer panel, lap and position readouts.
 * Positions and sprite sources from the WW.EXE HUD pass (FUN_0002a92c and
 * friends); see docs/SPRITES_HUD.md.
 */
#include "wacky.h"

#include <stdlib.h>
#include <string.h>

#define MINIMAP_W   78
#define MINIMAP_H   50
#define MINIMAP_X    0
#define MINIMAP_Y   60
#define MINIMAP_SZ  (MINIMAP_W * MINIMAP_H)      /* 0xF3C per track */

#define SPEEDO_X     0
#define SPEEDO_Y   181
#define SPEEDO_W    42
#define SPEEDO_H    19
#define ICONS_SPEEDO 0xCCC

struct WHud {
    const uint8_t *map;        /* this track's 78x50 minimap bitmap */
    const uint8_t *mapx, *mapy; /* MAP.XY: world offset -> minimap pixel */
    const uint8_t *icons;
    uint32_t icons_len;
    int shown_speed;           /* eased display value */
};

/* cup map files, 5 tracks each in the order the game races them */
static const struct { const char *file; int first; } CUPS[] = {
    { "BRONZEM.SP", 1 }, { "SILVERM.SP", 6 }, { "GOLDM.SP", 11 },
};

WHud *whud_create(const WDat *dat, int tracknum) {
    WHud *h = calloc(1, sizeof *h);
    if (!h) return NULL;
    uint32_t len;
    const uint8_t *xy = wdat_find(dat, "MAP.XY", &len);
    if (xy && len >= 4096) { h->mapx = xy; h->mapy = xy + 2048; }
    h->icons = wdat_find(dat, "ICONS.SP", &h->icons_len);

    for (size_t c = 0; c < sizeof CUPS / sizeof *CUPS; c++) {
        int idx = tracknum - CUPS[c].first;
        if (idx < 0 || idx > 4) continue;
        const uint8_t *m = wdat_find(dat, CUPS[c].file, &len);
        if (m && len >= (uint32_t)((idx + 1) * MINIMAP_SZ))
            h->map = m + (size_t)idx * MINIMAP_SZ;
        break;
    }
    return h;
}

void whud_free(WHud *h) { free(h); }

static void put(uint32_t *fb, const uint8_t dac[768], int x, int y, uint8_t idx) {
    if (x < 0 || x >= WW_SCREEN_W || y < 0 || y >= WW_SCREEN_H) return;
    fb[(size_t)y * WW_SCREEN_W + x] =
        0xFF000000u | (uint32_t)dac[idx * 3 + 2] << 16 |
        (uint32_t)dac[idx * 3 + 1] << 8 | dac[idx * 3];
}

/* opaque blit of a column-major sprite */
static void blit(uint32_t *fb, const uint8_t dac[768], const uint8_t *src,
                 int w, int h, int x0, int y0, bool keyed) {
    if (!src) return;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t px = src[x * h + y];
            if (keyed && px == WW_SP_TRANSPARENT) continue;
            put(fb, dac, x0 + x, y0 + y, px);
        }
}

void whud_draw(uint32_t *fb, WHud *h, const WTrack *t, const WPhys *p,
               const WAi *ai) {
    if (!h) return;

    /* --- minimap (FUN_0002a4ac): 78x50 at (0,60), row-major bitmap --- */
    if (h->map) {
        for (int y = 0; y < MINIMAP_H; y++)
            for (int x = 0; x < MINIMAP_W; x++) {
                uint8_t px = h->map[x * MINIMAP_H + y];   /* column-major */
                if (px == WW_SP_TRANSPARENT) continue;    /* track floats */
                put(fb, t->dac, MINIMAP_X + x, MINIMAP_Y + y, px);
            }
        /* kart dots: world offset through MAP.XY, own kart last so it wins */
        if (h->mapx && h->mapy) {
            for (int k = 7; k >= 0; k--) {
                int wx, wy, compass;
                uint8_t colour;
                if (k == 0) {
                    wx = p->worldx;
                    wy = p->worldy;
                    colour = 0xFF;
                } else {
                    if (!ai) continue;
                    wai_kart_state(ai, k, &wx, &wy, &compass);
                    colour = 0x5A;
                }
                wx -= WW_MAP_OFF;
                wy -= WW_MAP_OFF;
                if (wx < 0) wx = 0;
                if (wx > 0x7FF) wx = 0x7FF;
                if (wy < 0) wy = 0;
                if (wy > 0x7FF) wy = 0x7FF;
                put(fb, t->dac, MINIMAP_X + h->mapx[wx],
                    MINIMAP_Y + h->mapy[wy], colour);
            }
        }
    }

    /* --- speedometer panel (ICONS.SP + 0xCCC) at (0,181) --- */
    if (h->icons && h->icons_len >= ICONS_SPEEDO + SPEEDO_W * SPEEDO_H) {
        /* displayed value eases toward 2 * speed: +2/+4 up, -4/-10 down */
        int target = p->speed * 2;
        if (h->shown_speed < target)
            h->shown_speed += (target - h->shown_speed > 8) ? 4 : 2;
        else if (h->shown_speed > target)
            h->shown_speed -= (h->shown_speed - target > 20) ? 10 : 4;
        if (h->shown_speed < 0) h->shown_speed = 0;
        blit(fb, t->dac, h->icons + ICONS_SPEEDO, SPEEDO_W, SPEEDO_H,
             SPEEDO_X, SPEEDO_Y, false);
    }
}

int whud_speed_shown(const WHud *h) { return h ? h->shown_speed : 0; }
