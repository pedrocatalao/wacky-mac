/* Game flow: logos, title, menus, selects, results and the championship.
 *
 * Ported from WW.EXE:
 *  - intro sequence APOG1 -> BEAVIS -> WINTRO with APOGEE/MAINMENU music
 *    (FUN_000347b4 region);
 *  - menu items from the executable's string table ("SINGLE PLAYER RACING",
 *    "AMATEUR CLASS", "SIX LAP RACE", "BRONZE RACE", ...);
 *  - championship scoring (race-end handler at 0x317xx): only the top three
 *    finishers score, 9/6/3 amateur, 12/9/6 pro, 15/12/9 champ;
 *  - the points screen draws on ORDER.PCX with the big font at 11px advance
 *    (FUN_0002bff0);
 *  - WFONT1.SP is 42 glyphs of 15x13 column-major: A-Z 0-9 space . / - ! TM.
 */
#include "wacky.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum Flow {
    FL_LOGO_APOGEE, FL_LOGO_BEAVIS, FL_TITLE,
    FL_MAIN, FL_CLASS, FL_LAPS, FL_CAR, FL_CUP,
    FL_PREVIEW, FL_RACE, FL_RESULTS, FL_BOARD, FL_PODIUM,
};

#define KART_W 38
#define KART_H 28

struct WMenu {
    const WDat *dat;
    int state;
    int frame;              /* frames spent in the current state */
    int fade;               /* 0..32 palette fade-in */
    /* current fullscreen background */
    WImage bg;
    char bg_name[16];
    /* fonts */
    const uint8_t *wfont;   /* WFONT1.SP: 42 x 15x13 column-major */
    const uint8_t *ofont;   /* OFONT.SP 8x8, chunk 1 */
    const uint8_t *cars;    /* CARS.SP */
    /* selections */
    int sel;                /* cursor in the current menu */
    int cls;                /* 1 amateur, 2 pro, 3 champ  */
    int laps;               /* 6 / 8 / 10 */
    int character;          /* 0..7 */
    int cup;                /* 0..2 */
    int race_idx;           /* 0..4 within the cup */
    int car_rot;            /* rotation animation on the select screen */
    /* championship standings */
    int points[8];          /* by kart index (0 = player) */
    int last_place[8];
    int player_kart;
    /* handoff to the race loop */
    int race_request;
    int quit;
    char cur_song[16];      /* dedupe so screen changes don't restart music */
};

/* the original keeps a song playing across related screens; only start a
 * different one (music placement per the preloaded slot table at 0x7c648:
 * MAINMENU, LEADRBRD, GAMEOVER, SPACEY, ASHES) */
static void menu_music(WMenu *m, const char *base, int loop) {
    /* looped songs carry across screens; jingles always retrigger */
    if (loop && strcmp(m->cur_song, base) == 0) return;
    snprintf(m->cur_song, sizeof m->cur_song, "%s", base);
    if (loop) wsound_music(m->dat, base);
    else wsound_music_once(m->dat, base);
}

static const char *CHAR_NAME[8] = {
    "UNO", "SULTAN", "MORRIS", "PEGGLES", "RAZER", "RINGO", "BLOMBO", "TIGI",
};

/* menu item texts from the WW.EXE string table */
static const char *MAIN_ITEMS[6] = {
    "SINGLE PLAYER RACING", "TWO PLAYER RACE", "TWO PLAYER SHOOT OUT",
    "WACKY DUCK SHOOT", "GAME OPTIONS", "ORDERING INFO",
};
static const char *CLASS_ITEMS[3] = { "AMATEUR CLASS", "PRO CLASS", "CHAMP CLASS" };
static const char *LAP_ITEMS[3]   = { "SIX LAP RACE", "EIGHT LAP RACE", "TEN LAP RACE" };
static const char *CUP_ITEMS[3]   = { "BRONZE RACE", "SILVER RACE", "GOLD RACE" };

/* ---- drawing ----------------------------------------------------------- */

static uint32_t rgba(const uint8_t *pal, int idx) {
    return 0xFF000000u | (uint32_t)pal[idx * 3 + 2] << 16 |
           (uint32_t)pal[idx * 3 + 1] << 8 | pal[idx * 3];
}

static void draw_bg(WMenu *m, uint32_t *fb) {
    if (!m->bg.pixels) { memset(fb, 0, WW_SCREEN_W * WW_SCREEN_H * 4); return; }
    /* palette fade-in like the original's DAC ramp */
    uint8_t pal[768];
    for (int i = 0; i < 768; i++)
        pal[i] = (uint8_t)(m->bg.pal[i] * m->fade / 32);
    for (int i = 0; i < WW_SCREEN_W * WW_SCREEN_H; i++)
        fb[i] = rgba(pal, m->bg.pixels[i]);
}

static bool load_bg(WMenu *m, const char *name) {
    if (strcmp(m->bg_name, name) == 0) return true;
    uint32_t len;
    const uint8_t *d = wdat_find(m->dat, name, &len);
    if (!d) return false;
    WImage img;
    if (!wpcx_decode(d, len, &img)) return false;
    wimage_free(&m->bg);
    m->bg = img;
    snprintf(m->bg_name, sizeof m->bg_name, "%s", name);
    m->fade = 0;
    return true;
}

/* WFONT1 mapping: A-Z 0-9 then space . / - ! TM */
static int wfont_index(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= '0' && c <= '9') return 26 + c - '0';
    switch (c) {
    case ' ': return 36;
    case '.': return 37;
    case '/': return 38;
    case '-': return 39;
    case '!': return 40;
    }
    return -1;
}

/* big-font text, 11px advance (FUN_000146d4); highlight remaps the gold
 * ramp to white the way the selected item lights up */
static void wtext(WMenu *m, uint32_t *fb, const char *s, int x, int y, int bright) {
    if (!m->wfont) return;
    for (; *s; s++, x += 11) {
        int g = wfont_index((unsigned char)*s);
        if (g < 0) continue;
        const uint8_t *px = m->wfont + (size_t)g * 15 * 13;
        for (int c = 0; c < 15; c++)
            for (int r = 0; r < 13; r++) {
                uint8_t p = px[c * 13 + r];
                if (!p) continue;
                int sx = x + c, sy = y + r;
                if (sx < 0 || sx >= WW_SCREEN_W || sy < 0 || sy >= WW_SCREEN_H)
                    continue;
                uint32_t col = rgba(m->bg.pal, p);
                if (bright) {           /* lift toward white */
                    uint32_t r8 = (col & 0xFF) + 0x60, g8 = (col >> 8 & 0xFF) + 0x60,
                             b8 = (col >> 16 & 0xFF) + 0x60;
                    if (r8 > 255) r8 = 255;
                    if (g8 > 255) g8 = 255;
                    if (b8 > 255) b8 = 255;
                    col = 0xFF000000u | b8 << 16 | g8 << 8 | r8;
                }
                fb[sy * WW_SCREEN_W + sx] = col;
            }
    }
}

static void wtext_c(WMenu *m, uint32_t *fb, const char *s, int y, int bright) {
    wtext(m, fb, s, (WW_SCREEN_W - (int)strlen(s) * 11) / 2, y, bright);
}

/* small 8x8 in-race font (OFONT.SP chunk 1) for the long help lines,
 * mapped like FUN_000148f4 */
static int ofont_index(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 0x41;
    if (c >= '0' && c <= '9') return c - 0x16;
    switch (c) {
    case '!': return 0x24;
    case '.': case ':': return 0x35;
    case '?': return 0x34;
    }
    return -1;
}

static void otext(WMenu *m, uint32_t *fb, const char *s, int x, int y) {
    if (!m->ofont) return;
    for (; *s; s++, x += 8) {
        int g = ofont_index((unsigned char)*s);
        if (g < 0) continue;
        const uint8_t *px = m->ofont + (size_t)g * 0x40;
        for (int c = 0; c < 8; c++)
            for (int r = 0; r < 8; r++) {
                uint8_t p = px[c * 8 + r];
                if (!p) continue;
                int sx = x + c, sy = y + r;
                if (sx < 0 || sx >= WW_SCREEN_W || sy < 0 || sy >= WW_SCREEN_H)
                    continue;
                fb[sy * WW_SCREEN_W + sx] = rgba(m->bg.pal, p);
            }
    }
}

static void otext_c(WMenu *m, uint32_t *fb, const char *s, int y) {
    otext(m, fb, s, (WW_SCREEN_W - (int)strlen(s) * 8) / 2, y);
}

/* kart sprite from CARS.SP (column-major 38x28), transparent 0 */
static void draw_kart(WMenu *m, uint32_t *fb, int kart, int rot, int x, int y) {
    if (!m->cars) return;
    const uint8_t *px = m->cars + ((size_t)kart * 12 + rot) * KART_W * KART_H;
    for (int c = 0; c < KART_W; c++)
        for (int r = 0; r < KART_H; r++) {
            uint8_t p = px[c * KART_H + r];
            if (!p) continue;
            int sx = x + c, sy = y + r;
            if (sx < 0 || sx >= WW_SCREEN_W || sy < 0 || sy >= WW_SCREEN_H)
                continue;
            fb[sy * WW_SCREEN_W + sx] = rgba(m->bg.pal, p);
        }
}

/* ---- state helpers ----------------------------------------------------- */

static void enter(WMenu *m, int state) {
    m->state = state;
    m->frame = 0;
    m->sel = 0;
    switch (state) {
    case FL_LOGO_APOGEE:
        load_bg(m, "APOG1.PCX");
        menu_music(m, "APOGEE", 0);            /* one-shot fanfare */
        break;
    case FL_LOGO_BEAVIS:
        load_bg(m, "BEAVIS.PCX");
        menu_music(m, "MAINMENU", 1);          /* starts here, not at title */
        break;
    case FL_TITLE:
        load_bg(m, "WINTRO.PCX");
        menu_music(m, "MAINMENU", 1);
        break;
    case FL_MAIN:
    case FL_CLASS:
    case FL_LAPS:
    case FL_CAR:
        load_bg(m, "ORDER.PCX");
        menu_music(m, "MAINMENU", 1);
        break;
    case FL_CUP:
        load_bg(m, "ORDER.PCX");
        menu_music(m, "ASHES", 1);             /* the select boards' song */
        break;
    case FL_PREVIEW: {
        char name[16];
        snprintf(name, sizeof name, "SHRINK%d.PCX",
                 m->cup * 5 + m->race_idx + 1);
        if (!load_bg(m, name)) load_bg(m, "ORDER.PCX");
        break;                                 /* music keeps playing */
    }
    case FL_RESULTS:
        load_bg(m, "ORDER.PCX");
        menu_music(m, "ASHES", 1);             /* points screen (FUN_0002bff0) */
        break;
    case FL_BOARD: {
        char name[16];
        snprintf(name, sizeof name, "SB%d.PCX", m->cup + 1);
        if (!load_bg(m, name)) load_bg(m, "ORDER.PCX");
        menu_music(m, "LEADRBRD", 0);          /* one-shot (FUN_0002e4b8) */
        break;
    }
    case FL_PODIUM:
        load_bg(m, "WIN.PCX");
        menu_music(m, "LEADRBRD", 0);
        break;
    }
}

static void start_championship(WMenu *m) {
    memset(m->points, 0, sizeof m->points);
    m->race_idx = 0;
    enter(m, FL_PREVIEW);
}

/* ---- public API -------------------------------------------------------- */

WMenu *wmenu_create(const WDat *dat) {
    WMenu *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    m->dat = dat;
    m->wfont = wdat_find(dat, "WFONT1.SP", NULL);
    m->ofont = wdat_find(dat, "OFONT.SP", NULL);
    m->cars = wdat_find(dat, "CARS.SP", NULL);
    m->cls = 1;
    m->laps = 6;
    enter(m, FL_LOGO_APOGEE);
    return m;
}

void wmenu_free(WMenu *m) {
    if (!m) return;
    wimage_free(&m->bg);
    free(m);
}

bool wmenu_active(const WMenu *m) { return m && m->state != FL_RACE; }
bool wmenu_quit(const WMenu *m) { return m && m->quit; }

bool wmenu_race_request(WMenu *m, int *track, int *laps, int *character) {
    if (!m || !m->race_request) return false;
    m->race_request = 0;
    *track = m->cup * 5 + m->race_idx + 1;
    *laps = m->laps;
    *character = m->character;
    m->state = FL_RACE;
    m->cur_song[0] = 0;            /* the race loop owns the music now */
    return true;
}

/* the race loop reports the finishing place of every kart (1..8, kart 0 is
 * the player); scoring is the original's: top three only, tiered by class */
void wmenu_race_done(WMenu *m, const int place_of_kart[8]) {
    if (!m) return;
    static const int TIER[4][3] = {
        {0, 0, 0}, {9, 6, 3}, {12, 9, 6}, {15, 12, 9},
    };
    for (int k = 0; k < 8; k++) {
        m->last_place[k] = place_of_kart[k];
        int p = place_of_kart[k];
        if (p >= 1 && p <= 3) m->points[k] += TIER[m->cls][p - 1];
    }
    m->player_kart = m->character;
    enter(m, FL_RESULTS);
}

void wmenu_race_aborted(WMenu *m) {
    if (!m) return;
    enter(m, FL_MAIN);
}

/* character of a kart index, mirroring the scene renderer's swap */
static int char_of_kart(const WMenu *m, int k) {
    if (k == 0) return m->character;
    return k == m->character ? 0 : k;
}

void wmenu_key(WMenu *m, int sym) {
    if (!m) return;
    int fire = sym == SDLK_RETURN || sym == SDLK_SPACE;
    switch (m->state) {
    case FL_LOGO_APOGEE: enter(m, FL_LOGO_BEAVIS); break;
    case FL_LOGO_BEAVIS: enter(m, FL_TITLE); break;
    case FL_TITLE:
        if (sym == SDLK_ESCAPE) m->quit = 1;
        else enter(m, FL_MAIN);
        break;
    case FL_MAIN:
        if (sym == SDLK_UP) { m->sel = (m->sel + 5) % 6; wsound_play(0x10); }
        else if (sym == SDLK_DOWN) { m->sel = (m->sel + 1) % 6; wsound_play(0x10); }
        else if (fire) {
            if (m->sel == 0) { wsound_play(0x16); enter(m, FL_CLASS); }
            else wsound_play(WSND_NOAMMO);        /* not in this port yet */
        } else if (sym == SDLK_ESCAPE) enter(m, FL_TITLE);
        break;
    case FL_CLASS:
        if (sym == SDLK_UP) { m->sel = (m->sel + 2) % 3; wsound_play(0x10); }
        else if (sym == SDLK_DOWN) { m->sel = (m->sel + 1) % 3; wsound_play(0x10); }
        else if (fire) { m->cls = m->sel + 1; wsound_play(0x16); enter(m, FL_LAPS); }
        else if (sym == SDLK_ESCAPE) enter(m, FL_MAIN);
        break;
    case FL_LAPS:
        if (sym == SDLK_UP) { m->sel = (m->sel + 2) % 3; wsound_play(0x10); }
        else if (sym == SDLK_DOWN) { m->sel = (m->sel + 1) % 3; wsound_play(0x10); }
        else if (fire) {
            m->laps = 6 + m->sel * 2;
            wsound_play(0x16);
            enter(m, FL_CAR);
        } else if (sym == SDLK_ESCAPE) enter(m, FL_CLASS);
        break;
    case FL_CAR:
        if (sym == SDLK_LEFT) { m->character = (m->character + 7) % 8; wsound_play(0x10); }
        else if (sym == SDLK_RIGHT) { m->character = (m->character + 1) % 8; wsound_play(0x10); }
        else if (fire) {
            wsound_play(m->character);            /* the driver's voice */
            enter(m, FL_CUP);
        } else if (sym == SDLK_ESCAPE) enter(m, FL_LAPS);
        break;
    case FL_CUP:
        if (sym == SDLK_UP) { m->sel = (m->sel + 2) % 3; wsound_play(0x10); }
        else if (sym == SDLK_DOWN) { m->sel = (m->sel + 1) % 3; wsound_play(0x10); }
        else if (fire) {
            m->cup = m->sel;
            wsound_play(0x16);
            start_championship(m);
        } else if (sym == SDLK_ESCAPE) enter(m, FL_CAR);
        break;
    case FL_PREVIEW:
        if (sym == SDLK_ESCAPE) enter(m, FL_MAIN);
        else m->race_request = 1;
        break;
    case FL_RESULTS:
        if (fire || sym == SDLK_ESCAPE) enter(m, FL_BOARD);
        break;
    case FL_BOARD:
        if (fire || sym == SDLK_ESCAPE) {
            if (m->race_idx < 4) {
                m->race_idx++;
                enter(m, FL_PREVIEW);
            } else {
                enter(m, FL_PODIUM);
            }
        }
        break;
    case FL_PODIUM:
        enter(m, FL_MAIN);
        break;
    }
}

/* ---- per-frame rendering ----------------------------------------------- */

static void menu_list(WMenu *m, uint32_t *fb, const char *title,
                      const char **items, int n) {
    wtext_c(m, fb, title, 24, 0);
    for (int i = 0; i < n; i++)
        wtext_c(m, fb, items[i], 70 + i * 20, i == m->sel);
}

void wmenu_frame(WMenu *m, uint32_t *fb) {
    if (!m) return;
    m->frame++;
    if (m->fade < 32) m->fade++;
    draw_bg(m, fb);

    switch (m->state) {
    case FL_LOGO_APOGEE:
        /* the fanfare runs about nine seconds; any key skips */
        if (m->frame > 540) enter(m, FL_LOGO_BEAVIS);
        break;
    case FL_LOGO_BEAVIS:
        /* 0x110 ticks of the 136 Hz clock (FUN_000347b4) */
        if (m->frame > 120) enter(m, FL_TITLE);
        break;
    case FL_TITLE:
        if ((m->frame / 30) & 1)
            wtext_c(m, fb, "PRESS FIRE", 180, 1);
        break;
    case FL_MAIN:
        menu_list(m, fb, "WACKY WHEELS", MAIN_ITEMS, 6);
        break;
    case FL_CLASS:
        menu_list(m, fb, "CHOOSE YOUR CLASS", CLASS_ITEMS, 3);
        break;
    case FL_LAPS:
        menu_list(m, fb, "CHOOSE RACE LENGTH", LAP_ITEMS, 3);
        break;
    case FL_CUP:
        menu_list(m, fb, "CHOOSE YOUR RACE", CUP_ITEMS, 3);
        break;
    case FL_CAR: {
        otext_c(m, fb, "USE LEFT AND RIGHT CONTROLS TO CHOOSE", 12);
        otext_c(m, fb, "THEN PRESS FIRE TO SELECT THE CAR", 26);
        if (++m->car_rot >= 8 * 6) m->car_rot = 0;
        for (int k = 0; k < 8; k++) {
            int x = 30 + (k % 4) * 70, y = 64 + (k / 4) * 60;
            int rot = k == m->character ? m->car_rot / 6 : 4;
            draw_kart(m, fb, k, rot, x, y);
            if (k == m->character)
                wtext(m, fb, CHAR_NAME[k],
                      x + (KART_W - (int)strlen(CHAR_NAME[k]) * 11) / 2,
                      y + KART_H + 4, 1);
        }
        break;
    }
    case FL_PREVIEW:
        if ((m->frame / 30) & 1)
            wtext_c(m, fb, "PRESS FIRE", 4, 1);
        break;
    case FL_BOARD:
        /* the SB cup board (FUN_0002e4b8) is self-captioned; any key moves on */
        break;
    case FL_RESULTS: {
        wtext_c(m, fb, "POINTS", 4, 0);
        /* standings sorted by points, the original's bubble order */
        int order[8];
        for (int i = 0; i < 8; i++) order[i] = i;
        for (int i = 0; i < 8; i++)
            for (int j = i + 1; j < 8; j++)
                if (m->points[order[i]] < m->points[order[j]]) {
                    int t = order[i]; order[i] = order[j]; order[j] = t;
                }
        for (int i = 0; i < 8; i++) {
            int k = order[i];
            char line[40];
            snprintf(line, sizeof line, "%-8s %3d",
                     CHAR_NAME[char_of_kart(m, k)], m->points[k]);
            /* fixed columns like the original's two-column layout */
            int x = i < 4 ? 24 : 176;
            int y = 40 + (i % 4) * 24;
            wtext(m, fb, line, x, y, k == 0);
        }
        char msg[40];
        snprintf(msg, sizeof msg, "YOU FINISHED %d", m->last_place[0]);
        wtext_c(m, fb, msg, 150, 1);
        if ((m->frame / 30) & 1)
            wtext_c(m, fb, "PRESS FIRE", 180, 0);
        break;
    }
    case FL_PODIUM: {
        /* champion = most points */
        int best = 0;
        for (int k = 1; k < 8; k++)
            if (m->points[k] > m->points[best]) best = k;
        char line[48];
        snprintf(line, sizeof line, "%s IS THE OVERALL WINNER!",
                 CHAR_NAME[char_of_kart(m, best)]);
        wtext_c(m, fb, line, 160, 1);
        break;
    }
    }
}
