/* Items and weapons — transcription of FUN_00024364 (fire), FUN_0002d548
 * (spawn), FUN_00025e6c (step), FUN_0002602c (update) and the pickup rules
 * in FUN_00022bf4. Projectile sprites live in HOGMIS.SP.
 */
#include "wacky.h"

#include <stdlib.h>
#include <string.h>

#define PROJ_MAX   61
#define PROJ_FRAME 0xEA     /* 18*13 */

/* frame strips inside HOGMIS.SP */
#define HOG_EXPLODE 0x0000  /* 3 frames, impact burst */
#define HOG_W0      0x02BE  /* 3, weapon 0 hedgehog   */
#define HOG_T0B     0x057C  /* 1, weapon 3 hedgehog   */
#define HOG_T1      0x0666  /* 1, side shot           */
#define HOG_T2      0x0750  /* 1, rear drop           */
#define HOG_T3      0x083A  /* 1, rear drop           */
#define HOG_T4      0x0924  /* 1, rear drop           */
#define HOG_T5      0x1506  /* 3, big squasher        */

typedef struct {
    int owner, type, active, state, owner_overlap;
    int x, y, frame, frame_count;
    int range, speed, travelled;
    int angle;                 /* absolute; for types 0/5 the spread variant */
    const uint8_t *sprite;
} Proj;

struct WWeapons {
    Proj pool[PROJ_MAX];
    const uint8_t *hog;
    uint32_t hog_len;
    int live;                  /* player's outstanding projectiles */
    int spawn_angle, spawn_frames;
    const uint8_t *spawn_sprite;
};

WWeapons *wweap_create(const WDat *dat) {
    WWeapons *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    w->hog = wdat_find(dat, "HOGMIS.SP", &w->hog_len);
    return w;
}

void wweap_free(WWeapons *w) { free(w); }

void wweap_reset(WWeapons *w) {
    for (int i = 0; i < PROJ_MAX; i++) w->pool[i].active = 0;
    w->live = 0;
}

static int wrapa(int a) {
    while (a < 0) a += WW_ANGLES;
    while (a > 0x77F) a -= WW_ANGLES;
    return a;
}

static int32_t tscale(int32_t t, int32_t s) {
    return (int32_t)((uint32_t)(t * s) + 0x8000u) >> 16;
}

/* one movement step; doCollide==0 is the free step taken at spawn.
 * Returns hit code: 0 none, 1 wall, 2 object, 3 kart. */
static int proj_step(Proj *pr, const WTrack *t, const WTables *tb,
                     const WCollide *col, int shooter_angle, int do_collide,
                     int *hit_kart_idx, const WPhys *player, int *hit_player) {
    int ang;
    int stationary = 0;
    if (pr->type < 1 || pr->type == 5) {
        /* steerable: heading re-read from the shooter every tick */
        if (pr->angle == 0)      ang = shooter_angle;
        else if (pr->angle < 2)  ang = wrapa(shooter_angle - 0x1E);
        else                     ang = wrapa(shooter_angle + 0x1E);
    } else {
        stationary = pr->type > 1;      /* dropped mines never move again */
        ang = pr->angle;
    }

    int x1 = pr->x, y1 = pr->y;
    if (!do_collide || !stationary) {
        x1 = pr->x + tscale(tb->cosq[ang], pr->speed);
        y1 = pr->y + tscale(tb->sinq[ang], pr->speed);
    }
    if (!do_collide) {
        pr->x = x1;
        pr->y = y1;
        return 0;
    }

    /* Bresenham walk, stopping at the first hit */
    int x = pr->x, y = pr->y;
    int dx = x1 - x, sx = dx < 0 ? -1 : 1; if (dx < 0) dx = -dx;
    int dy = y1 - y, sy = dy < 0 ? -1 : 1; if (dy < 0) dy = -dy;
    int hit = 0;
    int steps = dy < dx ? dx : dy;
    int e = (dy < dx ? dx : dy) >> 1;
    for (int i = 0; i <= steps && !hit; i++) {
        if (ww_surface_at(t, (uint16_t)x, (uint16_t)y) == 3) { hit = 1; break; }
        if (col) {
            if (wscene_blocks(col->obj_ctx, x, y)) { hit = 2; break; }
            if (col->hit_kart) {
                int k = wai_kart_at(col->kart_ctx, x, y);
                if (k >= 0) {
                    if (hit_kart_idx) *hit_kart_idx = k;
                    hit = 3;
                    break;
                }
            }
        }
        /* the player is kart 0: same Manhattan 0x12 test, with the owner
         * immunity from ProjProbe — your own drop only arms once it is no
         * longer overlapping you, and types 0/5 never hit their owner */
        if (player) {
            int dx2 = player->worldx - x, dy2 = player->worldy - y;
            if (dx2 < 0) dx2 = -dx2;
            if (dy2 < 0) dy2 = -dy2;
            int in_range = (dx2 + dy2) < 0x12;
            if (pr->owner == 0) {
                if (in_range) {
                    if (pr->owner_overlap || pr->type == 0 || pr->type == 5)
                        in_range = 0;
                } else if (pr->owner_overlap) {
                    pr->owner_overlap = 0;      /* cleared it: now armed */
                }
            }
            if (in_range) {
                if (hit_player) *hit_player = 1;
                hit = 3;
                break;
            }
        }
        if (i == steps) break;
        if (dy < dx) { e += dy; if (e > dx) { y += sy; e -= dx; } x += sx; }
        else         { e += dx; if (e > dy) { x += sx; e -= dy; } y += sy; }
    }
    pr->x = hit ? x : x1;
    pr->y = hit ? y : y1;
    return hit;
}

static Proj *spawn(WWeapons *w, int type, int owner, int kart_x, int kart_y,
                   const WTrack *t, const WTables *tb, int shooter_angle) {
    for (int i = 0; i < PROJ_MAX; i++) {
        Proj *p = &w->pool[i];
        if (p->active) continue;
        p->speed = (type == 1 || type == 6) ? 0x10 : 10;
        p->owner_overlap = 1;
        p->state = 0;
        p->active = 1;
        p->range = 1000;
        p->type = type;
        p->owner = owner;
        p->angle = w->spawn_angle;
        p->x = kart_x;
        p->y = kart_y;
        proj_step(p, t, tb, NULL, shooter_angle, 0, NULL, NULL, NULL);  /* free step */
        p->travelled = 0;
        p->frame = 0;
        p->sprite = w->spawn_sprite;
        p->frame_count = w->spawn_frames;
        return p;
    }
    return NULL;
}

/* FUN_00024364 — fire button handling and weapon dispatch */
void wweap_fire(WWeapons *w, WPhys *p, const WTrack *t, const WTables *tb,
                bool fire_btn, int tick, int *sound_out) {
    if (!w->hog) return;
    int sfx = -1;

    if (!fire_btn) {
        p->last_fire_tick = tick;
        if (p->fire_latch) p->fire_latch = 0;
    } else {
        if (!p->fire_latch) { p->fire_latch = 1; p->last_fire_tick = tick; }
        else fire_btn = false;
        if (tick - p->last_fire_tick > 0x13) {      /* held: charge to weapon 8 */
            p->last_fire_tick = tick;
            if (p->weapon_id != 8) p->weapon_id = 8;
            fire_btn = false;
        }
    }
    if (!fire_btn) return;
    if (p->hop_state) return;
    if (w->live) return;
    if (p->weapon_id == 0 && p->ammo == 0) { if (sound_out) *sound_out = 0x1A; return; }

    int wid = p->weapon_id;
    if (wid > 8) return;
    int rear = wrapa(p->angle + 0x3C0);

    switch (wid) {
    case 0:
        w->spawn_frames = 3; w->spawn_sprite = w->hog + HOG_W0; w->spawn_angle = 0;
        spawn(w, 0, 0, p->worldx, p->worldy, t, tb, p->angle);
        sfx = 0x14;
        w->live = 1;
        break;
    case 3:
        for (int v = 0; v < 3; v++) {
            w->spawn_frames = 1; w->spawn_sprite = w->hog + HOG_T0B; w->spawn_angle = v;
            spawn(w, 0, 0, p->worldx, p->worldy, t, tb, p->angle);
        }
        sfx = 0x13;
        w->live = 3;
        break;
    case 4:
        sfx = 0x17;
        for (int s = 1; s < 3; s++) {
            w->spawn_angle = wrapa(s == 1 ? p->angle + 0x1E0 : p->angle - 0x1E0);
            w->spawn_frames = 1; w->spawn_sprite = w->hog + HOG_T1;
            spawn(w, 1, 0, p->worldx, p->worldy, t, tb, p->angle);
        }
        w->live = 0;
        break;
    case 5: case 6: case 7: {
        static const int off[3] = { HOG_T2, HOG_T3, HOG_T4 };
        sfx = 0x17;
        w->spawn_angle = rear;
        w->spawn_frames = 1;
        w->spawn_sprite = w->hog + off[wid - 5];
        spawn(w, wid - 3, 0, p->worldx, p->worldy, t, tb, p->angle);
        w->live = 0;
        break;
    }
    case 8:
        w->spawn_frames = 3; w->spawn_sprite = w->hog + HOG_T5; w->spawn_angle = 0;
        spawn(w, 5, 0, p->worldx, p->worldy, t, tb, p->angle);
        sfx = 0x14;
        w->live = 0;
        break;
    default:
        return;                       /* ids 1,2 fire nothing */
    }

    if (sound_out) *sound_out = sfx;
    if (p->weapon_id == 0) p->ammo--;
    else                   p->weapon_id = 0;    /* specials are single use */
}

/* FUN_0002602c — per-tick projectile update */
void wweap_tick(WWeapons *w, const WTrack *t, const WTables *tb,
                const WCollide *col, WPhys *player, int tick) {
    int shooter_angle = player ? player->angle : 0;
    if (!w->hog) return;
    for (int i = 0; i < PROJ_MAX; i++) {
        Proj *pr = &w->pool[i];
        if (!pr->active) continue;

        pr->frame++;
        if (pr->frame >= pr->frame_count) {
            pr->frame = 0;
            if (pr->state == 0xFF) { pr->active = 0; continue; }
        }
        if (pr->state == 0xFF) continue;        /* exploding: no motion */

        int victim = -1, struck_player = 0;
        int used = pr->speed;
        int hit = proj_step(pr, t, tb, col, shooter_angle, 1, &victim,
                            player, &struck_player);

        if (pr->type == 1) {
            if (hit == 1 || hit == 2) {          /* side shots bounce 180 */
                pr->angle = wrapa(pr->angle + 0x3C0);
                pr->x += tscale(tb->cosq[pr->angle], pr->speed);
                pr->y += tscale(tb->sinq[pr->angle], pr->speed);
                hit = 0;
            }
        } else if (pr->type == 0 || pr->type == 5) {
            pr->speed = 100;                     /* 10 on the first tick */
            pr->travelled += used;
            if (pr->travelled >= pr->range) hit = 1;
        }

        if (hit) {
            if (hit == 3 && victim >= 0)
                wai_kart_hit(col->kart_ctx, victim, pr->type == 5 ? 2 : 1, tick);
            /* the player spins out when struck (human-victim branch of
             * ProjProbe); the spin machinery is already in the physics */
            if (struck_player && player && player->spin_dir == 0) {
                player->spin_dir = 1;
                player->spin_step = 0;
                player->spin_frame = 7;
                player->drift = 0;
                player->hold_l = player->hold_r = 0;
            }
            if (w->live > 0) w->live--;
            pr->state = 0xFF;
            pr->frame_count = 3;
            pr->frame = 0;
            pr->sprite = w->hog + HOG_EXPLODE;
        }
    }
}

/* enumeration for the renderer */
int wweap_enum(const WWeapons *w, int i, int *x, int *y, const uint8_t **sprite) {
    if (i < 0 || i >= PROJ_MAX) return 0;
    const Proj *p = &w->pool[i];
    if (!p->active || !p->sprite) return 0;
    *x = p->x;
    *y = p->y;
    *sprite = p->sprite + (size_t)p->frame * PROJ_FRAME;
    return 1;
}

int wweap_count(void) { return PROJ_MAX; }
