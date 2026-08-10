/* Player kart physics — direct transcription of the WW.EXE routines
 * (FUN_000290a0 / FUN_000224ec / FUN_000237e4 and helpers).
 * Constants, operation order and state fields follow the decompilation;
 * see docs/PHYSICS.md and the port notes. Omitted here: sprite-anim state
 * machine details, items, link/net play, demo modes, AI.
 */
#include "wacky.h"

#include <stdlib.h>
#include <string.h>

/* surface codes (verified in probe/response code) */
#define SURF_TURBO  2
#define SURF_WALL   3
#define SURF_WATER1 6
#define SURF_RAMP   10
#define SURF_WATER2 0xF

#define NOSE       0x7C     /* nose/probe arm */
#define WALL_FLIP  0x3C0    /* 180 deg */
#define WALL_PUSH  0x20
#define WHISK      0x12     /* whisker angle step */
#define SPIN_RATE  0x78
#define HOP_AIR_VEL_IDX 150

typedef struct {
    const WTrack  *track;
    const WTables *tb;
    const WCollide *col;
} PhysCtx;

static int wrapa(int a) {
    if (a < 0) return a + WW_ANGLES;
    if (a > 0x77F) return a - WW_ANGLES;
    return a;
}

/* trig_scale: ((t * s) + 0x8000) >> 16, 32-bit wraparound as shipped */
static int32_t tscale(int32_t t, int32_t s) {
    return (int32_t)((uint32_t)(t * s) + 0x8000u) >> 16;
}

void wphys_reset(WPhys *p, const WTrack *t) {
    memset(p, 0, sizeof *p);
    p->angle = 0x1E0;                        /* constant on every track */
    /* physics pos sits 0x7C behind the kart sprite along heading */
    p->posx = t->start_x[0];
    p->posy = t->start_y[0] - NOSE;
    p->speed = 0;
    p->throttle = 0;
}

static uint32_t surface_at(const PhysCtx *c, int32_t x, int32_t y) {
    if (x < 0) x = 0; if (x > 0xFFF) x = 0xFFF;
    if (y < 0) y = 0; if (y > 0xFFF) y = 0xFFF;
    return ww_surface_at(c->track, (uint16_t)x, (uint16_t)y);
}

/* ---- movement integrator (FUN_000237e4) ---- */

static void whisker_check(const PhysCtx *c, WPhys *p, int *hitL, int *hitR) {
    int aL = wrapa(p->angle + WHISK), aR = wrapa(p->angle - WHISK);
    int32_t lx = p->posx + tscale(c->tb->cosq[aL], NOSE);
    int32_t ly = p->posy + tscale(c->tb->sinq[aL], NOSE);
    int32_t rx = p->posx + tscale(c->tb->cosq[aR], NOSE);
    int32_t ry = p->posy + tscale(c->tb->sinq[aR], NOSE);
    *hitL = surface_at(c, lx, ly) == SURF_WALL;
    *hitR = surface_at(c, rx, ry) == SURF_WALL;
    /* both-or-neither cancellation (whisker guard, port note 1) */
    if ((*hitL && *hitR) || (!*hitL && !*hitR)) { *hitL = 0; *hitR = 0; }
}

static void whisker_respond(const PhysCtx *c, WPhys *p) {
    int hitL, hitR;
    whisker_check(c, p, &hitL, &hitR);
    if (hitL) {
        if (p->scrape_state == 0) { p->scrape_state = 4; p->scrape_cnt = 0; }
        p->angle = wrapa(p->angle - WHISK);
        p->posx += tscale(c->tb->cosq[p->angle], 1);
        p->posy += tscale(c->tb->sinq[p->angle], 1);
        p->scraping = 1;
    }
    if (hitR) {
        if (p->scrape_state == 0) { p->scrape_state = 3; p->scrape_cnt = 0; }
        p->angle = wrapa(p->angle + WHISK);
        p->posx += tscale(c->tb->cosq[p->angle], 1);
        p->posy += tscale(c->tb->sinq[p->angle], 1);
        p->scraping = 1;
    }
}

/* record which side the thing we hit is on, and whether it is nearer or
 * farther than our own draw distance (0x7C) — the codes FUN_00022bf4 puts in
 * the display-list entry and FUN_0002d3fc consumes. */
static void note_bump(const PhysCtx *c, WPhys *p, int ox, int oy, int is_kart) {
    int32_t dx = ox - p->posx, dy = oy - p->posy;
    int32_t cq = c->tb->cosq[p->angle], sq = c->tb->sinq[p->angle];
    int32_t z  = (cq * dx + sq * dy) >> 16;      /* depth   */
    int32_t lx = (dy * cq - dx * sq) >> 16;      /* lateral */
    p->bump_pending = 1;
    p->bump_kart = is_kart;
    p->bump_horz = lx > 0 ? 5 : (lx < 0 ? 4 : 3);
    p->bump_vert = z > NOSE ? 1 : (z < NOSE ? 2 : 3);
}

/* probe one walk step at walk pos + nose offset; sets p->collide */
static void probe_step(const PhysCtx *c, WPhys *p,
                       int32_t wx, int32_t wy, int32_t nx, int32_t ny) {
    uint32_t s = surface_at(c, wx + nx, wy + ny);
    if (s == SURF_RAMP && p->hop_state == 0) {       /* jump ramp */
        if (p->in_water) { p->in_water = 0; p->splash = 0; }
        p->hop_state = 1;
        p->hop_air = 8;
        p->hop_height = 8;
        p->hop_maxh = 0x28;
        p->collide = 4;
        return;
    }
    if (s == SURF_WALL) { p->collide = 1; return; }

    /* objects then other karts, Manhattan distance < 0xE (probe_step) */
    if (p->hop_state == 0 && c->col) {
        int px = wx + nx, py = wy + ny;
        int ox = px, oy = py;
        if (c->col->hit_object &&
            c->col->hit_object(c->col->obj_ctx, px, py, &ox, &oy)) {
            p->object_hit = 1;
            p->collide = 2;
            note_bump(c, p, ox, oy, 0);
            return;
        }
        if (c->col->hit_kart) {
            int k = c->col->hit_kart(c->col->kart_ctx, px, py, &ox, &oy);
            if (k) {
                p->object_hit = 1;
                p->collide = 3;
                p->collide_kart = k - 1;
                note_bump(c, p, ox, oy, 1);
            }
        }
    }
}

static void walk_path(const PhysCtx *c, WPhys *p,
                      int32_t nx, int32_t ny, int32_t tx, int32_t ty) {
    int32_t x = p->posx, y = p->posy;
    int32_t dx = tx - x, sx = dx < 0 ? -1 : 1; if (dx < 0) dx = -dx;
    int32_t dy = ty - y, sy = dy < 0 ? -1 : 1; if (dy < 0) dy = -dy;
    int32_t err;
    if (dy < dx) {
        err = dx >> 1;
        while (x != tx) {
            probe_step(c, p, x, y, nx, ny);
            if (p->collide) break;
            err += dy; if (dx < err) { y += sy; err -= dx; }
            x += sx;
            p->posx = x; p->posy = y;
        }
    } else {
        err = dy >> 1;
        while (y != ty) {
            probe_step(c, p, x, y, nx, ny);
            if (p->collide) break;
            err += dx; if (dy < err) { x += sx; err -= dy; }
            y += sy;
            p->posx = x; p->posy = y;
        }
    }
    if (!p->collide) probe_step(c, p, p->posx, p->posy, nx, ny);
}

/* rotate about the point ndist[0] ahead, keeping it fixed (the steering
 * pivot, also used by the race-start camera swing) */
void wphys_pivot_turn(WPhys *p, const WTables *tb, int amount, int dir) {
    int32_t r = tb->ndist[0];
    int32_t px = tscale(tb->cosq[p->angle], r);
    int32_t py = tscale(tb->sinq[p->angle], r);
    p->angle = wrapa(dir > 0 ? p->angle + amount : p->angle - amount);
    p->posx += px - tscale(tb->cosq[p->angle], r);
    p->posy += py - tscale(tb->sinq[p->angle], r);
}

static void steer_pivot(const PhysCtx *c, WPhys *p, int amount, int dir) {
    /* pivot = pos + vec(angle, ndist[0]); rotate; pos += pivot_old - pivot_new */
    int32_t r = c->tb->ndist[0];
    int32_t px = tscale(c->tb->cosq[p->angle], r);
    int32_t py = tscale(c->tb->sinq[p->angle], r);
    p->angle = wrapa(dir > 0 ? p->angle + amount : p->angle - amount);
    p->posx += px - tscale(c->tb->cosq[p->angle], r);
    p->posy += py - tscale(c->tb->sinq[p->angle], r);
}

static void player_move(const PhysCtx *c, WPhys *p) {
    p->skid = 0;

    /* nose offset from PRE-steer angle */
    int move_angle = p->angle;
    int32_t nx = tscale(c->tb->cosq[move_angle], NOSE);
    int32_t ny = tscale(c->tb->sinq[move_angle], NOSE);
    int32_t tx = p->posx + tscale(c->tb->cosq[move_angle], p->speed);
    int32_t ty = p->posy + tscale(c->tb->sinq[move_angle], p->speed);
    p->collide = 0;
    walk_path(c, p, nx, ny, tx, ty);

    if (p->collide && p->hop_state) {
        if (p->collide == 1) p->hop_state = 0;   /* wall cancels hop */
        else p->collide = 0;                     /* fly over everything else */
    }
    if (p->collide || p->drift) p->skid = 1;

    /* bump response (FUN_00022bf4 tail): any object/kart contact cancels a
     * drift or spin and zeroes the throttle if one was active; the kart is
     * NOT otherwise slowed — it just stopped at the contact point */
    if (p->collide == 2 || p->collide == 3) {
        if (p->drift)    { p->drift = 0;    p->throttle = 0; }
        if (p->spin_dir) { p->spin_dir = 0; p->throttle = 0; }
        p->scraping = 1;
        /* sparks on the side that was hit: horzRel 5 -> right, else left */
        p->scrape_state = p->bump_horz == 5 ? 4 : 3;
        p->scrape_cnt = 0;
    }

    if (p->collide == 1) {
        /* wall: stop, push back 0x20 along flipped facing, whiskers, throttle 0 */
        if (p->drift) { p->drift = 0; p->throttle = 0; }
        if (p->spin_dir) { p->spin_dir = 0; p->throttle = 0; }
        int flipped = wrapa(move_angle + WALL_FLIP);
        p->posx += tscale(c->tb->cosq[flipped], WALL_PUSH);
        p->posy += tscale(c->tb->sinq[flipped], WALL_PUSH);
        whisker_respond(c, p);
        p->throttle = 0;
        p->speed = 0;
    } else {
        if (p->steer_l >= 1) steer_pivot(c, p, p->steer_l, -1);
        if (p->steer_r >= 1) steer_pivot(c, p, p->steer_r, +1);
        whisker_respond(c, p);
    }

    /* final nose probe + surface latch */
    int32_t fx = p->posx + tscale(c->tb->cosq[p->angle], NOSE);
    int32_t fy = p->posy + tscale(c->tb->sinq[p->angle], NOSE);
    uint32_t s = surface_at(c, fx, fy);
    p->surface = s;
    p->worldx = fx;
    p->worldy = fy;
    if ((int)s < c->tb->sdx_count) {
        p->drag = c->tb->sdx_drag[s];
        p->grip = c->tb->sdx_grip[s];
    } else {
        p->drag = 0;
        p->grip = 0;
    }

    /* water entry: codes 6 and 0xF */
    if ((s == SURF_WATER1 || s == SURF_WATER2) &&
        p->hop_state == 0 && !p->in_water) {
        p->in_water = 1;
        p->splash = 1;
    }
}

/* ---- steering / drift / spin (FUN_000224ec) ---- */

static void decay_speed(WPhys *p) {
    if (p->speed > 0 && p->drag != 0)
        p->speed -= (int16_t)(((uint32_t)((int32_t)p->drag * p->speed)) >> 16);
}

static void player_steer(const PhysCtx *c, WPhys *p, int steer_rate,
                         bool keyL, bool keyR) {
    (void)c;
    int drift_rate = steer_rate >> 1;

    /* hop-turn override: 0x78/tick for 5 ticks */
    if (p->hop_turn_dir != 0) {
        if (p->hop_turn_dir == 1) { p->steer_l = SPIN_RATE; p->steer_r = 0; }
        else                      { p->steer_r = SPIN_RATE; p->steer_l = 0; }
        if (++p->hop_turn_cnt >= 5) { p->hop_turn_dir = 0; p->drift = 0; }
        return;
    }

    if (p->spin_dir == 0) {
        if (p->drift == 0) {
            /* normal steering + drift trigger */
            if (p->speed > 0 && p->drag != 0) {
                decay_speed(p);
                if (p->speed < 0) p->speed = 0;
            }
            bool drift_ok = p->throttle > 99 && !p->in_water &&
                            p->surface != SURF_TURBO;
            p->steer_l = keyL ? steer_rate : 0;
            if (keyR) { p->steer_l = 0; p->steer_r = steer_rate; }
            else p->steer_r = 0;
            if (!drift_ok) return;

            if (p->hold_l < p->grip) {
                if (p->hold_r < p->grip) return;
                /* right drift trigger */
                p->steer_l = p->steer_r = 0;
                if (keyR) p->steer_r = drift_rate;
                p->hold_l = p->hold_r = 0;
                p->drift = 2;
                p->drift_timer = 0x78;
                if (p->grip != 0) return;
                /* zero grip: instant spin right */
                p->drift = 0; p->steer_r = 0; p->spin_dir = 2;
                p->spin_step = 0; p->spin_frame = 9;
            } else {
                /* left drift trigger */
                p->steer_l = p->steer_r = 0;
                if (keyL) p->steer_l = drift_rate;
                p->drift = 1;
                p->drift_timer = 0x78;
                p->hold_l = p->hold_r = 0;
                if (p->grip != 0) return;
                p->drift = 0; p->steer_r = 0; p->spin_dir = 1;
                p->spin_step = 0; p->spin_frame = 7;
            }
            return;
        }

        /* drift active */
        int release = p->grip == 0 ? 0 : p->grip * 2;
        p->steer_l = 0; p->steer_r = 0;
        if (keyL) {
            p->steer_l = drift_rate;
            if (p->drift == 2) {
                p->steer_l = 0;
                if (p->hold_l > 1 && p->grip != 0) { p->drift = 0; return; }
            }
        }
        if (keyR) {
            p->steer_r = drift_rate;
            if (p->drift == 1) {
                p->steer_r = 0;
                if (p->hold_r > 1 && p->grip != 0) { p->drift = 0; return; }
            }
        }
        if (p->drift == 1) {
            if (release <= p->hold_l) {          /* held too long: spin out */
                p->drift = 0; p->spin_dir = 1; p->spin_step = 0; p->spin_frame = 7;
                p->steer_l = SPIN_RATE;
            }
        } else if (release <= p->hold_r) {
            p->drift = 0; p->spin_dir = 2; p->spin_step = 0; p->spin_frame = 9;
            p->steer_r = SPIN_RATE;
        }
        p->drift_timer -= 10;
        if (p->drift_timer < 0) p->drift = 0;
        decay_speed(p);
        if (p->speed > 0 && p->drift != 0) return;
        if (p->spin_dir == 0) { p->steer_l = 0; p->steer_r = 0; }
        p->hold_l = p->hold_r = 0;
        p->drift = 0;
        p->speed = 0;
        return;
    }

    /* spin-out: rotate 0x78/tick for 8-ish steps, drag decays speed */
    p->steer_l = 0; p->steer_r = 0;
    if (p->spin_dir == 1) {
        p->steer_l = SPIN_RATE;
        if (--p->spin_frame < 0) goto spin_done;
    } else {
        p->steer_r = SPIN_RATE;
        if (++p->spin_frame > 0x10) goto spin_done;
    }
    decay_speed(p);
    if (p->speed < 1) goto spin_done;
    return;
spin_done:
    p->spin_dir = 0;
    p->hold_l = p->hold_r = 0;
    p->steer_l = p->steer_r = 0;
    p->throttle = 0;
}

/* ---- per-tick entry (FUN_000290a0) ---- */

void wphys_tick(WPhys *p, const WTrack *t, const WTables *tb,
                const WPhysInput *in, int detail_level, const WCollide *col) {
    PhysCtx c = { t, tb, col };
    static const int rates[4] = { 0x28, 0x28, 0x24, 0x20 };
    int steer_rate = rates[detail_level & 3];

    /* bump deflection (FUN_0002d3fc), applied before this tick's physics:
     * turn 0x12 away from the side hit, step 1 unit along the new heading,
     * and for kart hits shove the throttle by 0x28 depending on whether the
     * other kart was nearer (boost, cap 0xBE) or farther (brake, floor 0) */
    if (p->bump_pending) {
        if (p->bump_horz == 5) p->angle = wrapa(p->angle - WHISK);
        else                   p->angle = wrapa(p->angle + WHISK);
        p->posx += tscale(tb->cosq[p->angle], 1);
        p->posy += tscale(tb->sinq[p->angle], 1);
        if (p->bump_kart) {
            if (p->bump_vert == 2) {
                p->throttle += 0x28;
                if (p->throttle > 0xBE) p->throttle = 0xBE;
            } else if (p->bump_vert == 1) {
                p->throttle -= 0x28;
                if (p->throttle < 0) p->throttle = 0;
            }
        }
        p->bump_pending = 0;
    }

    /* scrape spark animation: one frame per tick, 4 frames then done
     * (tail of FUN_00028998) */
    if (p->scrape_state) {
        if (++p->scrape_cnt >= 4) {
            p->scrape_state = 0;
            p->scrape_cnt = 0;
        }
    }

    p->steer_l = 0; p->steer_r = 0;
    bool accel = in->accel, brake = in->brake, hop = in->hop;
    bool keyL = in->left, keyR = in->right;

    /* input squelch while spinning / hop-turning */
    if (p->spin_dir || p->hop_turn_dir) {
        keyL = keyR = false;
        accel = brake = hop = false;
    }

    if (p->hop_state == 0) {
        /* steer-hold counters */
        if (!keyL) p->hold_l = 0;
        else { p->hold_r = 0; p->hold_l++; }
        if (!keyR) p->hold_r = 0;
        else { p->hold_l = 0; p->hold_r++; }

        /* brake / accel / coast / turbo timing */
        if (accel && p->turbo) accel = false;
        if (brake && p->throttle > 0) {
            accel = false;
            p->throttle -= 8;
            if (p->throttle < 0) p->throttle = 0;
            p->speed = tb->vel[p->throttle];
        }
        if (!accel && p->engine_state > 2) p->engine_state = 0;
        if (!accel && !p->turbo) {
            p->throttle -= 4;
            if (p->throttle < 0) p->throttle = 0;
        } else if (!p->turbo) {
            p->throttle += 10;
            if (p->throttle > 100) p->throttle = 100;
            if (p->engine_state == 0) { p->engine_state = 1; p->engine_anim = 0; }
        } else {
            if (--p->turbo_timer < 1) p->turbo = 0;
        }
        p->speed = tb->vel[p->throttle];

        /* hop-turn trigger (hop button while moving) */
        if (p->drift == 0 && p->spin_dir == 0 && p->throttle != 0 &&
            hop && p->hop_turn_dir == 0) {
            p->hop_turn_cnt = 1;
            p->hop_turn_dir = keyR ? 2 : 1;
            p->drift = p->hop_turn_dir;
        }
        if (p->surface != SURF_RAMP)
            player_steer(&c, p, steer_rate, keyL, keyR);
    } else {
        /* airborne: fixed speed, no steering */
        p->speed = tb->vel[HOP_AIR_VEL_IDX < 200 ? HOP_AIR_VEL_IDX : 199];
        keyL = keyR = false;
        p->hop_air--;
        if (p->hop_air < 5) p->hop_state = 2;
        /* hop height (visual; original updates in draw) */
        if (p->hop_state == 1) {
            p->hop_height += 8;
            if (p->hop_height > p->hop_maxh) p->hop_height = p->hop_maxh;
        } else if (p->hop_state == 2) {
            p->hop_height -= 8;
            if (p->hop_height <= 0) { p->hop_height = 0; p->hop_state = 0; }
        }
    }

    /* integrate */
    player_move(&c, p);

    /* turbo tile (surface 2) */
    if (p->surface == SURF_TURBO && p->turbo == 0) {
        p->turbo = 1;                     /* heading window check omitted (TODO) */
        p->throttle = 0xA0;
        p->turbo_timer = 0x10;
        p->speed = tb->vel[p->throttle];
    } else if (p->throttle < 1) {
        p->turbo = 0;
    }

    /* exhaust animation (FUN_000261fc): frames 0..3; every 4 ticks the engine
     * state advances, so the accel smoke only shows briefly after each press
     * (engine_state > 1 stops it drawing; releasing accel re-arms it) */
    p->engine_anim++;
    if (p->engine_anim > 3) {
        p->engine_anim = 0;
        if (p->engine_state != 0) p->engine_state++;
    }

    /* water splash decay */
    if (p->splash && ++p->splash > 5) p->splash = 0;
    if (p->in_water && p->surface != SURF_WATER1 && p->surface != SURF_WATER2)
        p->in_water = 0;
}
