/* OpenWacky — faithful Wacky Wheels engine (SDL2 front end).
 *
 * Algorithms and constants lifted from WW.EXE disassembly — see docs/.
 *   openwacky [path/to/WACKY.DAT] [track#]
 *
 * Controls: arrows drive, X hop/handbrake, SPACE/Z fire, N/P track, C kart,
 *           M debug map, R reset, ESC quit.
 */
#include <SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wacky.h"

#define WIN_SCALE  4
#define KART_W     38
#define KART_H     28
#define NUM_KARTS   8
#define NUM_TRACKS  42

/* authentic timing: 136 Hz master clock, 12-tick frame floor (docs/PHYSICS.md) */
#define TICK_MS    (12.0 * 1000.0 / 136.0)   /* ≈ 88.24 ms */

/* authentic physics constants */
#define ACCEL_IDX     10
#define COAST_IDX      4
#define BRAKE_IDX      8
#define IDX_CAP      100
#define TURBO_IDX    160
#define TURBO_TICKS   16
#define TURN_HIGH     40      /* detail-level high */
#define HOP_TURN     120
#define HOP_TICKS      5
#define NOSE_DIST    124
#define PIVOT_DIST   133      /* NDIST[0][0] */
#define WHISKER_ANG   18
#define WALL_PUSH     32

/* ---- authentic renderer (docs/RENDERER.md) ---- */

static uint32_t fb[WW_SCREEN_W * WW_SCREEN_H];
static WTables TB;

static int wrap_angle(int a) {
    a %= WW_ANGLES;
    return a < 0 ? a + WW_ANGLES : a;
}

static uint32_t pal_rgba(const uint8_t pal[768], uint8_t idx) {
    return 0xFF000000u | (uint32_t)pal[idx * 3 + 2] << 16 |
           (uint32_t)pal[idx * 3 + 1] << 8 | pal[idx * 3];
}

static int sky_off = 0;     /* byte offset 0..240 into the panorama (4px units) */
static int sky_vel = 0;

static void render_view(const WTrack *t, const WPhys *p, bool left, bool right) {
    /* rows 0..109: backdrop indices through the (possibly cycling) race palette */
    if (t->back)
        for (int i = 0; i < WW_SKY_Y0 * WW_SCREEN_W; i++)
            fb[i] = pal_rgba(t->dac, t->back[i]);

    /* rows 110..129: N.PAR panorama window, velocity-scrolled (docs) */
    if (left || right) {
        if (sky_vel < 4) sky_vel++;
        sky_off += (right ? sky_vel : -sky_vel);
    } else {
        sky_vel = 0;
    }
    if (sky_off < 0) sky_off += 160;         /* two copies of 640px in strip */
    if (sky_off > 240) sky_off -= 160;
    int px_off = sky_off * 4;
    for (int y = 0; y < WW_SKY_ROWS; y++) {
        uint32_t *dst = fb + (size_t)(WW_SKY_Y0 + y) * WW_SCREEN_W;
        const uint8_t *src = t->par + (size_t)y * WW_PAR_W;
        for (int x = 0; x < WW_SCREEN_W; x++)
            dst[x] = pal_rgba(t->dac, src[(x + px_off) % WW_PAR_W]);
    }

    /* rows 130..199: column ray-caster, one TRIG entry per column */
    for (int col = 0; col < WW_SCREEN_W; col++) {
        int a = wrap_angle(p->angle - 160 + col);
        int32_t cq = TB.cosq[a], sq = TB.sinq[a];
        const int32_t *nd = TB.ndist + (size_t)col * WW_GROUND_ROWS;
        for (int k = 0; k < WW_GROUND_ROWS; k++) {
            int32_t d = nd[k];
            uint16_t wx = (uint16_t)(p->posx + ((cq * d) >> 16));
            uint16_t wy = (uint16_t)(p->posy + ((sq * d) >> 16));
            uint8_t ti = ww_tile_at(t, wx, wy);
            uint8_t pix = t->tiles[ti][(wy & 31) * WW_TILE + (wx & 31)];
            fb[(size_t)(WW_SCREEN_H - 1 - k) * WW_SCREEN_W + col] = pal_rgba(t->dac, pix);
        }
    }
}

/* engine effect overlay (exhaust smoke / dust): per-surface EFFECTS.SP frame
 * drawn at the kart's display position + (xoff, 0x10) — WW.EXE FUN_00024bcc */
static void draw_effect(const uint8_t *frame, const uint8_t dac[768],
                        int w, int h, int sx, int sy) {
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t px = frame[x * h + y];       /* column-major source */
            if (px == WW_SP_TRANSPARENT) continue;
            int py = sy + y, pxx = sx + x;
            if (py < 0 || py >= WW_SCREEN_H || pxx < 0 || pxx >= WW_SCREEN_W) continue;
            fb[(size_t)py * WW_SCREEN_W + pxx] =
                0xFF000000u | (uint32_t)dac[px * 3 + 2] << 16 |
                (uint32_t)dac[px * 3 + 1] << 8 | dac[px * 3];
        }
}

/* player kart blit: native 38x28 column-major frame, centered, ground row 192 */
static void draw_kart_raw(const uint8_t *frame, const uint8_t dac[768], int y_lift) {
    if (!frame) return;
    int x0 = WW_SCREEN_W / 2 - KART_W / 2, y0 = 192 - KART_H - y_lift;
    for (int y = 0; y < KART_H; y++)
        for (int x = 0; x < KART_W; x++) {
            uint8_t px = frame[x * KART_H + y];      /* column-major source */
            if (px == WW_SP_TRANSPARENT) continue;
            int sy = y0 + y;
            if (sy < 0 || sy >= WW_SCREEN_H) continue;
            fb[(size_t)sy * WW_SCREEN_W + x0 + x] =
                0xFF000000u | (uint32_t)dac[px * 3 + 2] << 16 |
                (uint32_t)dac[px * 3 + 1] << 8 | dac[px * 3];
        }
}

/* ---- race-start sequence (FUN_0002fd8c @23157) ---- */
enum StartPhase { START_SLIDE, START_SWING, START_LIGHT, RACING };
static enum StartPhase phase = START_SLIDE;
static int32_t start_x, start_y;
static int light_tick;

static void intro_arm(WPhys *p, WScene *scene, const WTrack *t, int kart_id) {
    start_x = p->posx + 0x84;
    start_y = p->posy;
    p->posy += 0x84;
    p->posx = 0x708;
    p->angle = 0x3C0;
    phase = START_SLIDE;
    light_tick = 0;
    if (scene) {
        wscene_set_light(scene, 0, 1);
        wscene_set_player_sprite(scene, 1, t->start_x[0], t->start_y[0],
                                 0x1E0, kart_id);
    }
}

/* one intro step; returns true while the intro still owns the tick */
static bool intro_step(WPhys *p, const WTables *tb, WScene *scene) {
    if (phase == RACING) return false;
    switch (phase) {
    case START_SLIDE:
        p->posx -= 0x40;
        if (p->posx <= start_x) { p->posx = start_x; phase = START_SWING; }
        break;
    case START_SWING:
        wphys_pivot_turn(p, tb, 0x28, -1);
        if (p->angle < 0x1E1) {
            p->posx = start_x - 0x84;
            p->posy = start_y;
            p->angle = 0x1E0;
            phase = START_LIGHT;
        }
        break;
    default:
        light_tick++;
        if (light_tick >= 0x22) {
            if (scene) {
                wscene_set_light(scene, 2, 1);
                wscene_set_player_sprite(scene, 0, 0, 0, 0, 0);
            }
            phase = RACING;
        } else if (scene) {
            wscene_set_light(scene, light_tick > 0x10 ? 1 : 0, 1);
        }
        break;
    }
    return true;
}

static void render_map_debug(const WTrack *t, const WPhys *p) {
    /* 4096 world / 20 ≈ full map fit; sample every 20.5 px */
    for (int y = 0; y < WW_SCREEN_H; y++)
        for (int x = 0; x < WW_SCREEN_W; x++) {
            uint16_t wx = (uint16_t)(x * WW_WORLD / WW_SCREEN_W);
            uint16_t wy = (uint16_t)(y * WW_WORLD / WW_SCREEN_H);
            uint8_t ti = ww_tile_at(t, wx, wy);
            fb[(size_t)y * WW_SCREEN_W + x] =
                pal_rgba(t->dac, t->tiles[ti][(wy & 31) * WW_TILE + (wx & 31)]);
        }
    int mx = p->posx * WW_SCREEN_W / WW_WORLD, my = p->posy * WW_SCREEN_H / WW_WORLD;
    for (int d = -2; d <= 2; d++) {
        if (mx + d >= 0 && mx + d < WW_SCREEN_W)
            fb[(size_t)my * WW_SCREEN_W + mx + d] = 0xFF0000FF;
        if (my + d >= 0 && my + d < WW_SCREEN_H)
            fb[(size_t)(my + d) * WW_SCREEN_W + mx] = 0xFF0000FF;
    }
}

int main(int argc, char **argv) {
    const char *dat_path = argc > 1 ? argv[1] : "WACKY.DAT";
    int tracknum = argc > 2 ? atoi(argv[2]) : 1;
    const char *dump_path = argc > 3 ? argv[3] : NULL;   /* render 1 frame to .ppm */

    WDat dat;
    if (!wdat_open(&dat, dat_path)) {
        fprintf(stderr, "cannot open archive: %s\n", dat_path);
        return 1;
    }
    if (!wtables_load(&TB, &dat)) {
        fprintf(stderr, "engine tables missing from archive\n");
        return 1;
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window *win = SDL_CreateWindow("OpenWacky",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        WW_SCREEN_W * WIN_SCALE, WW_SCREEN_H * WIN_SCALE,
        SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE);
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_RenderSetLogicalSize(ren, WW_SCREEN_W, WW_SCREEN_H);
    SDL_RenderSetIntegerScale(ren, SDL_TRUE);
    SDL_Texture *screen_tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888,
        SDL_TEXTUREACCESS_STREAMING, WW_SCREEN_W, WW_SCREEN_H);

    WTrack track;
    WSprite cars = {0};
    WScene *scene = NULL;
    WAi *ai = NULL;
    WWeapons *weap = wweap_create(&dat);
    const uint8_t *cars_raw = wdat_find(&dat, "CARS.SP", NULL);
    /* per-character sprite files: lean frames [0..1] left, [2..3] right */
    static const char *char_names[8] = {
        "PANDA.SP", "CAMEL.SP", "MOOSE.SP", "PELICAN.SP",
        "SHARK.SP", "RINGO.SP", "ELE.SP", "TIGER.SP"
    };
    const uint8_t *char_raw[8];
    for (int i = 0; i < 8; i++)
        char_raw[i] = wdat_find(&dat, char_names[i], NULL);
    int steer_anim = 2;      /* 0..4, 2 = straight (SPR_IDLE animFrame) */
    /* exhaust sprite sets live inside HOGMIS.SP: +0xA0E idle, +0xDB6 accel,
     * +0x115E turbo; 18x13 frames of 0xEA bytes */
    const uint8_t *hogmis = wdat_find(&dat, "HOGMIS.SP", NULL);
    /* collision sparks: 4 frames of 10x9 */
    const uint8_t *spark = wdat_find(&dat, "SPARK.SP", NULL);
    int tick_no = 0;
    WPhys player;
    int cyc_cnt60 = 0, cyc_cnt50 = 0, cyc_ph_a = 0, cyc_ph_b = 0;
    int lap = 1, prev_prog = 0, wrong_way = 0;
    int kart_id = 0;
    bool map_view = false;
    bool need_load = true, running = true;
    double acc_ms = TICK_MS;   /* render first frame immediately */
    Uint64 prev = SDL_GetPerformanceCounter();

    while (running) {
        if (need_load) {
            if (cars.rgba) { wsprite_free(&cars); wtrack_free(&track); }
            if (!wtrack_load(&track, &dat, tracknum)) {
                fprintf(stderr, "failed to load track %d\n", tracknum);
                return 1;
            }
            if (!wsprite_load(&cars, &dat, "CARS.SP", KART_W, KART_H, track.dac)) {
                fprintf(stderr, "failed to load CARS.SP\n");
                return 1;
            }
            wscene_free(scene);
            scene = wscene_load(&dat, &track, tracknum);
            wai_free(ai);
            ai = wai_load(&dat, &track, tracknum);
            if (ai) wai_reset(ai, &track, 1, 1, &TB);
            if (weap) wweap_reset(weap);
            wphys_reset(&player, &track);
            player.ammo = 4;     /* the original starts you with hedgehogs */
            intro_arm(&player, scene, &track, kart_id);

            /* headless pre-simulation for frame dumps: argv[4] = tick count */
            if (dump_path && argc > 4) {
                int pre = atoi(argv[4]);
                bool st = pre < 0;               /* negative = steer left */
                if (st) pre = -pre;
                WPhysInput in = { true, false, st, false, false };
                WCollide pcol = { wscene_hit_object, wai_hit_kart, scene, ai };
                for (int i = 0; i < pre; i++) {
                    if (intro_step(&player, &TB, scene)) continue;
                    wphys_tick(&player, &track, &TB, &in, 1, &pcol);
                    if (weap) {
                        wweap_fire(weap, &player, &track, &TB, i == 0, i, NULL);
                        wweap_tick(weap, &track, &TB, &pcol, &player, i);
                    }
                    if (scene) wscene_tick(scene);
                    if (ai) {
                        wai_tick(ai, &TB, 0, 1);
                        wai_progress(ai, &track);
                    }
                }
            }
            /* drop the time spent loading, or the accumulator would fire a
             * burst of catch-up ticks and eat the whole intro fly-in */
            prev = SDL_GetPerformanceCounter();
            acc_ms = 0;
            cyc_cnt60 = track.cycle_on ? 1 : 0;   /* GAM line 25 seeds counter */
            cyc_cnt50 = 0;
            cyc_ph_a = 0;
            cyc_ph_b = 0;
            need_load = false;
        }

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) running = false;
            else if (ev.type == SDL_KEYDOWN && !ev.key.repeat) {
                switch (ev.key.keysym.sym) {
                case SDLK_ESCAPE: running = false; break;
                case SDLK_n: tracknum = tracknum % NUM_TRACKS + 1; need_load = true; break;
                case SDLK_p: tracknum = (tracknum + NUM_TRACKS - 2) % NUM_TRACKS + 1; need_load = true; break;
                case SDLK_c: kart_id = (kart_id + 1) % NUM_KARTS; break;
                case SDLK_m: map_view = !map_view; break;
                case SDLK_r: wphys_reset(&player, &track); break;
                }
            }
        }

        Uint64 now = SDL_GetPerformanceCounter();
        acc_ms += (double)(now - prev) * 1000.0 / (double)SDL_GetPerformanceFrequency();
        prev = now;
        /* the original runs exactly one logic tick per rendered frame, paced
         * by a busy-wait floor — never a burst of catch-up ticks */
        if (acc_ms > TICK_MS) acc_ms = TICK_MS;

        const Uint8 *keys = SDL_GetKeyboardState(NULL);
        if (dump_path) { acc_ms = TICK_MS; }   /* deterministic single tick */
        bool accel = keys[SDL_SCANCODE_UP] || dump_path != NULL;
        bool brake = keys[SDL_SCANCODE_DOWN];
        bool left = keys[SDL_SCANCODE_LEFT], right = keys[SDL_SCANCODE_RIGHT];
        bool hop = keys[SDL_SCANCODE_X];
        bool fire = keys[SDL_SCANCODE_SPACE] || keys[SDL_SCANCODE_Z];

        bool ticked = false;
        if (acc_ms >= TICK_MS) {             /* one tick per frame, 11.34 Hz */
            if (intro_step(&player, &TB, scene)) {
                acc_ms -= TICK_MS;
                ticked = true;
                continue;
            }

            WPhysInput in = { accel, brake, left, right, hop };
            WCollide col = { wscene_hit_object, wai_hit_kart, scene, ai };
            wphys_tick(&player, &track, &TB, &in, 1, &col);
            wscene_resolve_pickups(scene, &player);
            /* ramming (FUN_00022bf4): at full throttle, not skidding and not
             * airborne, hitting an opponent whose camera-relative octant is
             * 0/1/7 (you caught it from behind) destroys it — and takes you
             * out too: the original zeroes the player's throttle and sets the
             * out-of-race flag that runs the crash camera. */
            if (ai && player.collide == 3 && player.throttle > 99 &&
                !player.skid && player.hop_state == 0) {
                int kx, ky, compass;
                wai_kart_state(ai, player.collide_kart, &kx, &ky, &compass);
                int cam_oct = player.angle / 240;
                if (player.angle % 240 > 0x77) cam_oct++;
                int rel = (compass - (cam_oct & 7)) & 7;
                if (rel == 0 || rel == 1 || rel == 7) {
                    wai_kart_ram(ai, player.collide_kart);
                    player.throttle = 0;
                    player.speed = 0;
                    player.drift = 0;
                    player.spin_dir = 0;
                }
            }
            player.collide = 0;
            if (weap) {
                wweap_fire(weap, &player, &track, &TB, fire, tick_no, NULL);
                wweap_tick(weap, &track, &TB, &col, &player, tick_no);
            }
            /* steering-lean animation: step toward target 1 frame/tick */
            {
                int target = left ? 0 : right ? 4 : 2;
                if (steer_anim < target) steer_anim++;
                else if (steer_anim > target) steer_anim--;
            }
            tick_no++;
            if (scene) wscene_tick(scene);
            if (ai) {
                int32_t pprog = (int32_t)(lap - 1) * track.pos_max + prev_prog;
                wai_tick(ai, &TB, pprog, 1);
                wai_progress(ai, &track);
            }

            /* lap / wrong-way from the N.POS progress grid */
            int prog = ww_progress_at(&track, player.worldx, player.worldy);
            if (prog > 0 && track.pos_max > 4) {
                if (prev_prog > (int)track.pos_max * 3 / 4 &&
                    prog < (int)track.pos_max / 4)
                    lap++;                          /* crossed the finish line */
                else if (prog > (int)track.pos_max * 3 / 4 &&
                         prev_prog > 0 && prev_prog < (int)track.pos_max / 4)
                    lap--;                          /* backed over the line */
                wrong_way = (prog < prev_prog - 1 && !(prev_prog > (int)track.pos_max * 3 / 4));
                if (prog != prev_prog) prev_prog = prog;
            }
            /* color cycling driver (WW.EXE FUN_00028668): cnt60 doubles as the
             * enable flag (GAM line 25) and the group-A tick counter */
            if (cyc_cnt60 != 0) {
                if (cyc_cnt50 >= track.cycle_per_b) {
                    cyc_ph_b = (cyc_ph_b + 1) & 3;
                    ww_palette_cycle_b(&track, TB.beach_pal, cyc_ph_b);
                    cyc_cnt50 = 0;
                }
                if (cyc_ph_a == 3 && cyc_cnt60 >= track.cycle_hold_a) {
                    cyc_ph_a = 4;
                    ww_palette_cycle_a(&track, TB.beach_pal, cyc_ph_a);
                    cyc_cnt60 = 0;
                } else if (cyc_cnt60 >= track.cycle_per_a && cyc_ph_a != 3) {
                    if (++cyc_ph_a > 8) cyc_ph_a = 0;
                    ww_palette_cycle_a(&track, TB.beach_pal, cyc_ph_a);
                    cyc_cnt60 = 0;
                }
                cyc_cnt60++;
                cyc_cnt50++;
            }
            acc_ms -= TICK_MS;
            ticked = true;
        }

        if (ticked || map_view) {
            if (map_view) {
                render_map_debug(&track, &player);
            } else {
                render_view(&track, &player, left, right);
                if (scene)
                    wscene_draw(fb, scene, &track, &TB, &player, cars_raw,
                                kart_id, ai, weap);
                /* 5-position steering animation (SPR_IDLE, anim state 5):
                 * 0,1 = char lean-left pair, 2 = CARS rear view,
                 * 3,4 = char lean-right pair; steps 1 frame/tick */
                const uint8_t *kf = NULL;
                if (phase != RACING) {
                    kf = NULL;      /* drawn as a world sprite during the intro */
                } else if (player.spin_dir && cars_raw) {
                    /* spin-out: the 17-entry spin table cycles the kart's own
                     * rotation frames starting at 4 (built at character select,
                     * FUN_00016ca0 region) */
                    int rot = (4 + player.spin_frame) & 7;
                    kf = cars_raw + ((size_t)kart_id * 12 + rot) * KART_W * KART_H;
                } else if (steer_anim == 2 || !char_raw[kart_id]) {
                    kf = cars_raw ? cars_raw + ((size_t)kart_id * 12 + 4) * KART_W * KART_H
                                  : NULL;
                } else {
                    int extra = steer_anim < 2 ? steer_anim : steer_anim - 1;
                    kf = char_raw[kart_id] + (size_t)extra * KART_W * KART_H;
                }
                /* speed bounce (+1px jitter while moving) — the original's
                 * bounce offset */
                int bounce = (player.speed > 0 && (tick_no & 1)) ? 1 : 0;
                int lift = player.hop_height + bounce;
                /* engine effect: drawn only while moving (speed != 0), frames
                 * cycle per tick; sprite set comes from the surface under the
                 * kart, so it becomes dust off-road */
                draw_kart_raw(kf, track.dac, lift);

                /* --- overlays, drawn after the kart (FUN_00024bcc order) --- */
                int dx0 = WW_SCREEN_W / 2 - KART_W / 2;   /* display x */
                int dy0 = 192 - KART_H - lift;            /* display y */
                bool overlays = player.hop_state == 0 && !player.in_water &&
                                player.spin_dir == 0 && player.drift == 0;

                /* exhaust (FUN_00024afc): 18x13, two copies 0xb apart at
                 * (x+5, y+0xb). Sprite set by engine state / throttle. */
                if (overlays && hogmis) {
                    const uint8_t *base = NULL;
                    if (player.engine_state == 0) {
                        if (player.throttle >= 0 && player.throttle <= 10)
                            base = hogmis + 0xA0E;              /* idle puff  */
                    } else if (player.engine_state <= 1) {
                        base = hogmis + 0xDB6;                  /* accel smoke */
                    }
                    if (player.throttle > 100) base = hogmis + 0x115E;  /* turbo flame */
                    if (base) {
                        const uint8_t *f = base + (size_t)player.engine_anim * 0xEA;
                        draw_effect(f, track.dac, 0x12, 0xD, dx0 + 5, dy0 + 0xB);
                        draw_effect(f, track.dac, 0x12, 0xD, dx0 + 5 + 0xB, dy0 + 0xB);
                    }
                }

                /* collision sparks (kart draw, scrapeState branch): 10x9 at
                 * the kart's left edge (state 3) or right edge +0x1E (4),
                 * displayY + 0x10 */
                if (spark && player.scrape_state) {
                    int sx = player.scrape_state == 3 ? dx0 : dx0 + 0x1E;
                    draw_effect(spark + (size_t)player.scrape_cnt * 0x5A,
                                track.dac, 10, 9, sx, dy0 + 0x10);
                }

                /* tire dust (per-surface, descriptor B): two copies at the rear
                 * wheels while moving; layout depends on the sprite height */
                uint32_t sf = player.surface;
                if (overlays && TB.effects && player.speed != 0 && sf < 64 &&
                    TB.sdx_effB_n[sf] > 0 && TB.sdx_effB_size[sf] > 0) {
                    int w = TB.sdx_effB_w[sf], h = TB.sdx_effB_h[sf];
                    uint32_t off = TB.sdx_effB_off[sf] +
                        (uint32_t)(tick_no % TB.sdx_effB_n[sf]) * TB.sdx_effB_size[sf];
                    int ex, ey, gap;
                    if (h == 0x18) { ex = dx0 + 1; ey = dy0 + 4;    gap = 0x18; }
                    else           { ex = dx0;     ey = dy0 + 0x14; gap = 0x1C; }
                    if (off + (uint32_t)(w * h) <= TB.effects_len) {
                        draw_effect(TB.effects + off, track.dac, w, h, ex, ey);
                        draw_effect(TB.effects + off, track.dac, w, h, ex + gap, ey);
                    }
                }

                /* skid/drift smoke (descriptor A): one copy at (x, y+0x10)
                 * while the drift/spin frames are showing */
                if (TB.effects && player.speed != 0 && sf < 64 &&
                    (player.drift || player.spin_dir) &&
                    TB.sdx_effA_n[sf] > 0 && TB.sdx_effA_size[sf] > 0) {
                    int w = TB.sdx_effA_w[sf], h = TB.sdx_effA_h[sf];
                    uint32_t off = TB.sdx_effA_off[sf] +
                        (uint32_t)(tick_no % TB.sdx_effA_n[sf]) * TB.sdx_effA_size[sf];
                    if (off + (uint32_t)(w * h) <= TB.effects_len)
                        draw_effect(TB.effects + off, track.dac, w, h,
                                    dx0, dy0 + 0x10);
                }
            }
            if (dump_path) {
                FILE *f = fopen(dump_path, "wb");
                if (f) {
                    fprintf(f, "P6\n%d %d\n255\n", WW_SCREEN_W, WW_SCREEN_H);
                    for (int i = 0; i < WW_SCREEN_W * WW_SCREEN_H; i++) {
                        uint8_t rgb[3] = { fb[i] & 0xFF, (fb[i] >> 8) & 0xFF,
                                           (fb[i] >> 16) & 0xFF };
                        fwrite(rgb, 1, 3, f);
                    }
                    fclose(f);
                }
                running = false;
            }
        }

        SDL_UpdateTexture(screen_tex, NULL, fb, WW_SCREEN_W * 4);
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, screen_tex, NULL, NULL);
        SDL_RenderPresent(ren);

        static Uint32 t0;
        if (SDL_GetTicks() - t0 > 1000) {
            char title[160];
            snprintf(title, sizeof title,
                     "OpenWacky — track %d lap %d%s kart %d idx %d speed %d angle %d (%d,%d)",
                     tracknum, lap, wrong_way ? " WRONG WAY" : "", kart_id,
                     player.throttle, player.speed, player.angle,
                     player.posx, player.posy);
            SDL_SetWindowTitle(win, title);
            t0 = SDL_GetTicks();
        }
    }

    wsprite_free(&cars);
    wtrack_free(&track);
    SDL_DestroyTexture(screen_tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    wdat_close(&dat);
    return 0;
}
