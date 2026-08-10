/* AI (CPU opponent) karts — transcription of FUN_00028cac / FUN_00028b20 /
 * FUN_00016f90 and the .RD route preprocessing (FUN_00021554).
 * AI karts are position-scripted along pre-baked 1px Bresenham streams; they
 * never steer, never probe surfaces. See docs + port notes.
 */
#include "wacky.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ROUTE 1024
#define STEP_BUF  (11000 * 2)   /* u16 pairs, 44000 bytes as original */

typedef struct {
    uint16_t *steps;      /* stepCount x {x,y} */
    uint16_t  step_count;
    uint16_t  compass;    /* display octant, pre-remapped in the file */
} RouteRec;

struct WAi {
    RouteRec route[MAX_ROUTE];
    int      route_last;          /* count-1 */
    uint16_t *stepbuf, *stepw;
    int      start_x, start_y;    /* route[0] first point */

    struct AiKart {
        int phase;                /* 0 launch, 1 start boost, 2 cruise, 3 rubber boost */
        int p0_ticks, p1_ticks;
        int cruise_speed, boost_speed, p0_speed;
        int steps_remaining;
        const uint16_t *stream;
        int route_idx;
        int lane_state, lane_offset;
        int x, y, compass;
        int lap, prev_cell;
        int32_t progress;
        int spin_state, hit_tick;   /* 1 = spin (0x21 ticks), 2 = squash (0x32) */
        int spin_frame;             /* rotation frame cycled while spinning    */
        int view_frame;             /* last frame drawn; spins start from it   */
        int hit_flag, squash_frame; /* 2 = rammed/destroyed, 4-frame squash    */
    } k[8];

    int top_idx;                  /* aiTopSpeedIdx */
    int rubber_margin;
};

static int bres_emit(WAi *ai, int x0, int y0, int x1, int y1) {
    int n = 0;
    int dx = x1 - x0, sx = dx < 0 ? -1 : 1; if (dx < 0) dx = -dx;
    int dy = y1 - y0, sy = dy < 0 ? -1 : 1; if (dy < 0) dy = -dy;
    if (dy < dx) {
        int err = dx >> 1;
        while (x0 != x1) {
            *ai->stepw++ = (uint16_t)x0; *ai->stepw++ = (uint16_t)y0; n++;
            err += dy; if (err > dx) { y0 += sy; err -= dx; }
            x0 += sx;
        }
    } else {
        int err = dy >> 1;
        while (y0 != y1) {
            *ai->stepw++ = (uint16_t)x0; *ai->stepw++ = (uint16_t)y0; n++;
            err += dx; if (err > dy) { x0 += sx; err -= dy; }
            y0 += sy;
        }
    }
    *ai->stepw++ = (uint16_t)x0; *ai->stepw++ = (uint16_t)y0;
    return n + 1;
}

WAi *wai_load(const WDat *dat, const WTrack *t, int tracknum) {
    char name[16];
    uint32_t len;
    snprintf(name, sizeof name, "%d.RD", tracknum);
    const uint8_t *rd = wdat_find(dat, name, &len);
    if (!rd || len < 2) return NULL;

    WAi *ai = calloc(1, sizeof *ai);
    if (!ai) return NULL;
    ai->stepbuf = malloc(STEP_BUF * 2 * sizeof(uint16_t));
    if (!ai->stepbuf) { free(ai); return NULL; }
    ai->stepw = ai->stepbuf;

    uint16_t count;
    memcpy(&count, rd, 2);
    if (count > MAX_ROUTE) count = MAX_ROUTE;
    ai->route_last = count - 1;
    for (int i = 0; i < count; i++) {
        uint16_t r[7];
        if (2 + (size_t)i * 14 + 14 > len) { ai->route_last = i - 1; break; }
        memcpy(r, rd + 2 + (size_t)i * 14, 14);
        int x0 = r[0] + WW_MAP_OFF, y0 = r[1] + WW_MAP_OFF;
        int x1 = r[2] + WW_MAP_OFF, y1 = r[3] + WW_MAP_OFF;
        if (i == 0) { ai->start_x = x0; ai->start_y = y0; }
        ai->route[i].compass = r[5];
        ai->route[i].steps = ai->stepw;
        ai->route[i].step_count = (uint16_t)bres_emit(ai, x0, y0, x1, y1);
        if ((size_t)(ai->stepw - ai->stepbuf) > STEP_BUF * 2 - 4096) break;
    }

    wai_reset(ai, t, 1 /* Amateur */, 1 /* 12hp */, NULL);
    return ai;
}

void wai_free(WAi *ai) {
    if (!ai) return;
    free(ai->stepbuf);
    free(ai);
}

static int iabs(int v) { return v < 0 ? -v : v; }

void wai_reset(WAi *ai, const WTrack *t, int class_id, int engine12,
               const WTables *tb) {
    static const int p1_ticks[8] = { 0x16, 0x1C, 0x1E, 0x22, 0x28, 0x32, 0x3A, 0x3C };
    switch (class_id) {
    default:
    case 1: ai->rubber_margin = 8;    ai->top_idx = engine12 ? 89 : 90; break;
    case 2: ai->rubber_margin = 4;    ai->top_idx = engine12 ? 94 : 92; break;
    case 3: ai->rubber_margin = 1;    ai->top_idx = engine12 ? 99 : 94; break;
    case 4: ai->rubber_margin = 0x14; ai->top_idx = 84; break;
    }
    /* start grid: shared y, x staggered 0x14; fill order 4,3,2,1,0,7,6,5 */
    int gx[8], gy[8];
    static const int order[8] = { 4, 3, 2, 1, 0, 7, 6, 5 };
    for (int s = 0; s < 8; s++) {
        gx[order[s]] = t->start_x[0] + 0x50 - 0x14 * s;
        gy[order[s]] = t->start_y[0];
    }
    for (int i = 0; i < 8; i++) {
        struct AiKart *k = &ai->k[i];
        memset(k, 0, sizeof *k);
        k->x = gx[i];
        k->y = gy[i];
        k->compass = ai->route[0].compass;
        k->route_idx = 0;
        int dy = iabs(ai->start_y - k->y);
        int dx = iabs(ai->start_x - k->x);
        k->steps_remaining = ai->route[0].step_count - dy;
        if (k->steps_remaining < 1) k->steps_remaining = 1;
        k->stream = ai->route[0].steps + (size_t)dy * 2;
        k->lane_offset = dx;
        k->lane_state = k->x < ai->start_x ? 3 : (k->x > ai->start_x ? 4 : 0);
        k->phase = 0;
        k->p0_ticks = 0x18;
        k->p1_ticks = p1_ticks[i];
        k->cruise_speed = ai->top_idx;                       /* index for now */
        k->boost_speed = ai->top_idx + 0x3C - 10 * (7 - i);
        k->p0_speed = ai->top_idx - 4 - 10 * (7 - i);        /* raw, as shipped */
        if (k->p0_speed < 1) k->p0_speed = 1;
    }
    /* index -> speed conversion (karts 1..7; kart 0 = player slot) */
    if (tb) {
        for (int i = 1; i < 8; i++) {
            struct AiKart *k = &ai->k[i];
            int b = k->boost_speed;
            k->boost_speed = tb->vel[b < 0 ? 0 : b > 199 ? 199 : b];
            k->cruise_speed = tb->vel[k->cruise_speed > 199 ? 199 : k->cruise_speed];
        }
    }
}

/* per tick: move karts 1..7; player progress passed for rubber banding */
void wai_tick(WAi *ai, const WTables *tb, int32_t player_progress, int player_rank) {
    for (int i = 1; i < 8; i++) {
        struct AiKart *k = &ai->k[i];
        /* destroyed: 4-frame squash animation, then the kart is gone
         * (FUN_000261fc + the kart draw's hitFlag == 2 branch) */
        if (k->hit_flag == 2) {
            if (k->squash_frame < 4) k->squash_frame++;
            continue;
        }
        /* hit karts stand still until the spin/squash animation ends
         * (FUN_000289ec: 0x21 ticks spinning, 0x32 squashed); while spinning
         * the sprite cycles through the kart's own 8 rotation frames */
        if (k->spin_state) {
            if (k->spin_state == 1) k->spin_frame = (k->spin_frame + 1) & 7;
            if (++k->hit_tick >= (k->spin_state == 1 ? 0x21 : 0x32)) {
                k->spin_state = 0;
                k->hit_tick = 0;
            }
            continue;
        }
        int steps;
        switch (k->phase) {
        case 0:
            steps = k->p0_speed;
            if (--k->p0_ticks < 1) k->phase = 1;
            break;
        case 1:
            steps = k->boost_speed;
            if (--k->p1_ticks < 1) k->phase = 2;
            break;
        default:
        case 2:
            steps = k->cruise_speed;
            /* rubber band (FUN_00028b20): karts 4..7 only */
            if (i > 3) {
                int32_t gap = player_progress - k->progress;
                if (gap < (7 - i) + ai->rubber_margin) {
                    /* close enough */
                } else {
                    static const int bump[8] = { 0, 0, 0, 0, 0x10, 0x1C, 0x24, 0x28 };
                    k->phase = 3;
                    k->p1_ticks = 32000;
                    k->boost_speed = tb->vel[ai->top_idx] + bump[i];
                    steps = k->boost_speed;
                }
                if (i > 4 && player_rank < 4) {
                    static const int push[4][8] = {
                        {0}, {0,0,0,0,0,3,4,5}, {0,0,0,0,0,3,2,1}, {0,0,0,0,0,2,1,0} };
                    steps += push[player_rank][i];
                }
            }
            break;
        case 3:
            steps = k->boost_speed;
            k->p1_ticks--;
            {
                int32_t gap = player_progress - k->progress;
                if (k->p1_ticks < 1 || gap <= (7 - i) + ai->rubber_margin)
                    k->phase = 2;
            }
            break;
        }

        int ofs = 0;
        if (k->lane_state != 0) {
            ofs = k->lane_state == 3 ? -k->lane_offset : k->lane_offset;
            k->lane_offset -= 4;
            if (k->lane_offset < 1) k->lane_state = 0;
        }

        /* anti-clump burst */
        if (k->lane_state == 0 && k->phase == 2) {
            for (int j = 7; j > 0; j--) {
                if (j == i) continue;
                if (iabs(ai->k[j].x - k->x) + iabs(ai->k[j].y - k->y) < 0x14) {
                    steps += 0x3C;
                    break;
                }
            }
        }

        for (int s = 0; s < steps; s++) {
            k->x = k->stream[0] + (s == 0 ? ofs : ofs);   /* x-only lane offset */
            k->y = k->stream[1];
            k->stream += 2;
            if (--k->steps_remaining <= 0) {
                if (++k->route_idx > ai->route_last) k->route_idx = 0;
                const RouteRec *rr = &ai->route[k->route_idx];
                k->compass = rr->compass;
                k->stream = rr->steps;
                k->steps_remaining = rr->step_count;
            }
        }
    }
}

/* lap/progress from the N.POS grid (falling edge max -> 1) */
void wai_progress(WAi *ai, const WTrack *t) {
    for (int i = 1; i < 8; i++) {
        struct AiKart *k = &ai->k[i];
        int cell = ww_progress_at(t, k->x, k->y);
        if (cell == 1 && k->prev_cell == t->pos_max) k->lap++;
        if (cell) k->prev_cell = cell;
        k->progress = (int32_t)k->lap * t->pos_max + cell;
    }
}

/* projectile probe: index of an AI kart within Manhattan 0x12, else -1 */
int wai_kart_at(void *ctx, int x, int y) {
    const WAi *ai = ctx;
    if (!ai) return -1;
    for (int i = 1; i < 8; i++) {
        if (ai->k[i].spin_state) continue;
        int dx = ai->k[i].x - x, dy = ai->k[i].y - y;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        if (dx + dy < 0x12) return i;
    }
    return -1;
}

void wai_kart_hit(void *ctx, int idx, int spin_state, int tick) {
    WAi *ai = ctx;
    (void)tick;
    if (!ai || idx < 1 || idx > 7) return;
    if (ai->k[idx].spin_state) return;
    ai->k[idx].spin_state = spin_state;
    ai->k[idx].hit_tick = 0;
    ai->k[idx].spin_frame = ai->k[idx].view_frame;   /* start from the frame
                                                        currently on screen */
}

/* collision probe: index+1 of an AI kart within Manhattan 0xE of (x,y) */
int wai_hit_kart(void *ctx, int x, int y, int *ox, int *oy) {
    const WAi *ai = ctx;
    if (!ai) return 0;
    for (int i = 1; i < 8; i++) {
        if (ai->k[i].hit_flag == 2) continue;
        int dx = ai->k[i].x - x, dy = ai->k[i].y - y;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        if (dx + dy < 0xE) {
            if (ox) *ox = ai->k[i].x;
            if (oy) *oy = ai->k[i].y;
            return i + 1;
        }
    }
    return 0;
}

/* rammed from behind at full throttle: kart is destroyed (hitFlag 2) */
void wai_kart_ram(WAi *ai, int idx) {
    if (!ai || idx < 1 || idx > 7) return;
    ai->k[idx].hit_flag = 2;
    ai->k[idx].squash_frame = 0;
    ai->k[idx].spin_state = 0;
}

/* remember the frame the renderer chose, so a spin starts from it */
void wai_set_view_frame(WAi *ai, int i, int frame) {
    if (ai && i >= 1 && i <= 7) ai->k[i].view_frame = frame;
}

/* render state: 0 normal (use compass), 1 spinning (frame = rotation frame),
 * 2 squashed (frame = squash frame 0..3), 3 hidden */
int wai_kart_render(WAi *ai, int i, int *x, int *y, int *compass, int *frame) {
    if (!ai || i < 1 || i > 7) return 3;
    struct AiKart *k = &ai->k[i];
    *x = k->x;
    *y = k->y;
    *compass = k->compass;
    if (k->hit_flag == 2) {
        if (k->squash_frame > 3) return 3;
        *frame = k->squash_frame;
        return 2;
    }
    if (k->spin_state == 1) {
        *frame = k->spin_frame;
        return 1;
    }
    return 0;
}

void wai_kart_state(const WAi *ai, int i, int *x, int *y, int *compass) {
    *x = ai->k[i].x;
    *y = ai->k[i].y;
    *compass = ai->k[i].compass;
}
