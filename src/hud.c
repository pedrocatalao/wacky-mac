/* In-race HUD — transcription of FUN_0002a92c and friends.
 * Sprite sources and screen positions from the HUD extraction; see the
 * table below. All sprites are column-major: byte[col*h + row].
 */
#include "wacky.h"

#include <stdlib.h>
#include <string.h>

/* ---- sprite sources ----------------------------------------------------
 * ICONS.SP +0x0000  6 x 26x21   power-up icons (weapon id 3..8)
 *          +0x0CCC  1 x 42x19   speedometer panel
 *          +0x0FEA 10 x 10x15   speed digits
 * LAP.SP   +0x0000  8 x 22x29   position sprites 1st..8th
 *          +0x2CDC  1 x 80x17   wrong-way banner frame A
 *          +0x322C  1 x 80x17   wrong-way banner frame B
 *          +0x377C  1 x 38x41   position plaque
 *          +0x3D92  1 x 65x19   label plate (lives)
 *          +0x4265  1 x 61x19   label plate (hedgehogs)
 *          +0x46EC 10 x 10x9    small digits
 * GENEF.SP +0x35A5 11 x 7x9     timer glyphs '0'..'9' and ':'
 * OFONT.SP  chunk1 61 x 8x8     in-race text font
 * -------------------------------------------------------------------- */
#define ICON_PICKUP(id) (h->icons + ((id) - 3) * 0x222)
#define SPEEDO_PANEL    (h->icons + 0x0CCC)
#define SPEED_DIGIT(d)  (h->icons + 0x0FEA + (d) * 150)
#define PLACE_SPRITE(p) (h->lap   + ((p) - 1) * 0x27E)
#define WRONGWAY(f)     (h->lap   + ((f) ? 0x322C : 0x2CDC))
#define PLAQUE          (h->lap   + 0x377C)
#define PLATE_LIVES     (h->lap   + 0x3D92)
#define PLATE_HOGS      (h->lap   + 0x4265)
#define SMALL_DIGIT(d)  (h->lap   + 0x46EC + (d) * 90)
#define TIMER_GLYPH(i)  (h->genef + 0x35A5 + (i) * 63)
#define FONT8(i)        (h->font  + (i) * 0x40)

#define MINIMAP_X 0
#define MINIMAP_Y 0x3C
#define MINIMAP_W 0x4E
#define MINIMAP_H 0x32
#define MINIMAP_SZ (MINIMAP_W * MINIMAP_H)

struct WHud {
    const uint8_t *map, *mapx, *mapy;
    const uint8_t *icons, *lap, *genef, *font;
    uint32_t icons_len, lap_len, genef_len, font_len;
    int shown_speed;
    int wrongway_phase;
};

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
    h->lap   = wdat_find(dat, "LAP.SP",   &h->lap_len);
    h->genef = wdat_find(dat, "GENEF.SP", &h->genef_len);
    h->font  = wdat_find(dat, "OFONT.SP", &h->font_len);  /* chunk 1 = 8x8 */

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

static uint32_t *FB;
static const uint8_t *DAC;

static void put(int x, int y, uint8_t idx) {
    if (x < 0 || x >= WW_SCREEN_W || y < 0 || y >= WW_SCREEN_H) return;
    FB[(size_t)y * WW_SCREEN_W + x] =
        0xFF000000u | (uint32_t)DAC[idx * 3 + 2] << 16 |
        (uint32_t)DAC[idx * 3 + 1] << 8 | DAC[idx * 3];
}

/* column-major blit; keyed skips index 0 */
static void blit(const uint8_t *src, int x, int y, int w, int hgt, bool keyed) {
    if (!src) return;
    for (int c = 0; c < w; c++)
        for (int r = 0; r < hgt; r++) {
            uint8_t p = src[c * hgt + r];
            if (keyed && p == 0) continue;
            put(x + c, y + r, p);
        }
}

/* the original always prints two digits, leading zero included */
static void dec2(int v, char out[3]) {
    if (v < 0) v = 0;
    if (v > 99) v = 99;
    out[0] = (char)('0' + v / 10);
    out[1] = (char)('0' + v % 10);
    out[2] = 0;
}

/* 8x8 in-race font mapping (FUN_000148f4); -1 = advance without drawing */
static int font8_index(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 0x41;
    if (c >= '0' && c <= '9') return c - 0x16;
    switch (c) {
    case '!': return 0x24; case '#': return 0x26; case '&': return 0x2A;
    case '\'': return 0x39; case '(': return 0x2C; case ')': return 0x2D;
    case ',': return 0x36; case '-': return 0x37;
    case '.': case ':': return 0x35;
    case '?': return 0x34; case '@': return 0x38;
    default: return -1;
    }
}

static void hud_text(const WHud *h, const char *s, int x, int y) {
    if (!h->font) return;
    for (; *s; s++, x += 8) {
        int g = font8_index((unsigned char)*s);
        if (g >= 0) blit(FONT8(g), x, y, 8, 8, true);
    }
}

/* MM:SS:T from the 10 Hz race counter (FUN_0002a618) */
static void timer_text(int32_t ticks, char out[8]) {
    if (ticks < 0) ticks = 0;
    if (ticks > 36000) ticks = 0;
    int tenths = (int)(ticks % 10);
    int32_t secs = ticks / 10;
    char d[3];
    dec2((int)(secs / 60), d); out[0] = d[0]; out[1] = d[1];
    out[2] = ':';
    dec2((int)(secs % 60), d); out[3] = d[0]; out[4] = d[1];
    out[5] = ':';
    out[6] = (char)('0' + tenths);
    out[7] = 0;
}

/* static chrome, drawn every frame here (the original blits it once per page) */
void whud_draw_static(uint32_t *fb, WHud *h, const WTrack *t) {
    if (!h) return;
    FB = fb; DAC = t->dac;
    if (h->lap && h->lap_len > 0x4265 + 61 * 19) {
        blit(PLAQUE,      0x11, 6, 0x26, 0x29, true);   /* (17,6)  38x41 */
        blit(PLATE_LIVES, 0x44, 6, 0x41, 0x13, true);   /* (68,6)  65x19 */
        blit(PLATE_HOGS,  0x92, 6, 0x3D, 0x13, true);   /* (146,6) 61x19 */
    }
}

void whud_draw(uint32_t *fb, WHud *h, const WTrack *t, const WPhys *p,
               const WAi *ai, const WHudState *st) {
    if (!h) return;
    FB = fb; DAC = t->dac;

    /* ---- minimap: track bitmap keyed, then 1x1 kart dots via MAP.XY ---- */
    if (h->map) {
        blit(h->map, MINIMAP_X, MINIMAP_Y, MINIMAP_W, MINIMAP_H, true);
        if (h->mapx && h->mapy) {
            for (int k = 7; k >= 0; k--) {
                int wx, wy, compass;
                uint8_t col;
                if (k == 0) { wx = p->worldx; wy = p->worldy; col = 0xFF; }
                else {
                    if (!ai) continue;
                    wai_kart_state(ai, k, &wx, &wy, &compass);
                    col = 0x5A;
                }
                wx -= WW_MAP_OFF; wy -= WW_MAP_OFF;
                if (wx < 0) wx = 0; if (wx > 0x7FF) wx = 0x7FF;
                if (wy < 0) wy = 0; if (wy > 0x7FF) wy = 0x7FF;
                put(h->mapx[wx], h->mapy[wy] + MINIMAP_Y, col);
            }
        }
    }

    /* ---- race timer: 7 glyphs of 7x9 at (264,112) ---- */
    if (h->genef) {
        char buf[8];
        timer_text(st->race_ticks, buf);
        int x = 0x108;
        for (int i = 0; i < 7; i++, x += 7)
            blit(TIMER_GLYPH((unsigned char)buf[i] - '0'), x, 0x70, 7, 9, true);
    }

    /* ---- speedometer: panel + two 10x15 digits at x 3 and 15, y 183 ---- */
    if (h->icons) {
        int target = p->speed * 2;
        int up = st->racing ? 2 : 4, down = st->racing ? 4 : 10;
        if (h->shown_speed < target) {
            h->shown_speed += up;
            if (h->shown_speed > target) h->shown_speed = target;
        } else if (h->shown_speed > target) {
            h->shown_speed -= down;
            if (h->shown_speed < target) h->shown_speed = target;
        }
        int v = st->racing ? h->shown_speed : 0;
        char d[3]; dec2(v, d);
        blit(SPEEDO_PANEL, 0, 0xB5, 0x2A, 0x13, false);
        blit(SPEED_DIGIT(d[0] - '0'),  3, 0xB7, 10, 15, false);
        blit(SPEED_DIGIT(d[1] - '0'), 15, 0xB7, 10, 15, false);
    }

    if (h->lap) {
        /* ---- position sprite 22x29 at (25,12) ---- */
        int place = st->place < 1 ? 1 : (st->place > 8 ? 8 : st->place);
        blit(PLACE_SPRITE(place), 0x19, 0x0C, 0x16, 0x1D, false);

        /* ---- hedgehog ammo: two 10x9 digits at (181,11) ---- */
        char d[3]; dec2(p->ammo, d);
        blit(SMALL_DIGIT(d[0] - '0'), 0xB5,      0x0B, 10, 9, false);
        blit(SMALL_DIGIT(d[1] - '0'), 0xB5 + 10, 0x0B, 10, 9, false);

        /* ---- lives: single 10x9 digit at (118,11), indexed by value ---- */
        int lives = st->lives < 0 ? 0 : (st->lives > 9 ? 9 : st->lives);
        blit(SMALL_DIGIT(lives), 0x76, 0x0B, 10, 9, false);

        /* ---- wrong-way banner 80x17 at (120,182), alternating frames ---- */
        if (st->wrong_way) {
            h->wrongway_phase = !h->wrongway_phase;
            blit(WRONGWAY(h->wrongway_phase), 0x78, 0xB6, 0x50, 0x11, true);
        }
    }

    /* ---- power-up icon 26x21 at (293,178) ---- */
    if (h->icons && p->weapon_id >= 3 && p->weapon_id <= 8)
        blit(ICON_PICKUP(p->weapon_id), 0x125, 0xB2, 0x1A, 0x15, true);

    /* ---- lap banner: 8x8 text at (4,164) ---- */
    {
        char line[16];
        if (st->lap < st->total_laps) {
            int n = 0;
            line[n++] = 'L'; line[n++] = 'A'; line[n++] = 'P'; line[n++] = ' ';
            if (st->lap >= 10) line[n++] = (char)('0' + st->lap / 10);
            line[n++] = (char)('0' + st->lap % 10);
            line[n] = 0;
        } else {
            memcpy(line, st->finished ? "FINISHED!" : "LAST LAP!", 10);
        }
        hud_text(h, line, 4, 0xA4);
    }
}

int whud_speed_shown(const WHud *h) { return h ? h->shown_speed : 0; }
