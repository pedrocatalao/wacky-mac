/* Sound effects — Creative Voice (.VOC) samples mixed through SDL audio.
 *
 * The id -> sample mapping is the game's own: a 27-entry table of 12-byte
 * name slots in WW.EXE's data segment, indexed directly by the id passed to
 * the play routine (FUN_00011c04). ids 0..7 are the character voices, which
 * is why the code plays "the driver's index" as a sound id.
 */
#include "wacky.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_VOICES 8

static const char *SOUND_NAME[WSND_COUNT] = {
    "UNO",   "SULTAN", "MORRIS", "PEGGLES", "RAZER", "RINGO", "BLOMBO", "TIGI",
    "PASS",  "BELL",   "START",  "GLASS",   "WARP",  "BOOM",  "HORN",   "SPLASH",
    "PLIP",  "SKID",   "PUF",    "FLAME",   "HOG",   "CLANG", "SWIPE",  "DROP",
    "BACKFIRE", "STALL1", "NO",
};

typedef struct {
    uint8_t *pcm;        /* 8-bit unsigned mono, resampled to the device rate */
    uint32_t len;
} Sample;

typedef struct {
    const Sample *s;
    uint32_t pos;
    int active;
} Voice;

/* the engine is a separate looping voice whose playback rate rises with
 * speed (the original streams MOTOR.VOC continuously and repitches it) */
typedef struct {
    Sample   smp;
    uint32_t pos_fp;      /* 16.16 position */
    uint32_t step_fp;     /* 16.16 step     */
    int      on;
} Engine;

struct WSound {
    SDL_AudioDeviceID dev;
    int rate;
    Sample smp[WSND_COUNT];
    Voice voice[MAX_VOICES];
    Engine eng;
    SDL_AudioSpec spec;
};

static WSound *G;

/* decode a VOC: header then blocks; type 1 = 8-bit unsigned PCM with a
 * time-constant rate divisor, type 9 = extended with an explicit rate */
static bool voc_decode(const uint8_t *d, uint32_t len, uint8_t **out,
                       uint32_t *out_len, int *rate) {
    if (len < 0x1A || memcmp(d, "Creative Voice File\x1a", 20) != 0) return false;
    uint16_t hdr;
    memcpy(&hdr, d + 20, 2);
    uint32_t p = hdr;
    uint8_t *pcm = NULL;
    uint32_t n = 0;
    *rate = 11025;
    while (p + 4 <= len) {
        uint8_t type = d[p];
        if (type == 0) break;
        uint32_t blen = (uint32_t)d[p + 1] | (uint32_t)d[p + 2] << 8 |
                        (uint32_t)d[p + 3] << 16;
        uint32_t body = p + 4;
        if (body + blen > len) break;
        if (type == 1 && blen >= 2) {
            int tc = d[body];
            if (d[body + 1] == 0) {           /* codec 0 = raw 8-bit */
                *rate = 1000000 / (256 - tc);
                uint32_t add = blen - 2;
                uint8_t *np = realloc(pcm, n + add);
                if (!np) { free(pcm); return false; }
                pcm = np;
                memcpy(pcm + n, d + body + 2, add);
                n += add;
            }
        } else if (type == 9 && blen >= 12) {
            uint32_t sr;
            memcpy(&sr, d + body, 4);
            if (sr) *rate = (int)sr;
            if (d[body + 4] == 8 && d[body + 5] == 1) {
                uint32_t add = blen - 12;
                uint8_t *np = realloc(pcm, n + add);
                if (!np) { free(pcm); return false; }
                pcm = np;
                memcpy(pcm + n, d + body + 12, add);
                n += add;
            }
        }
        p = body + blen;
    }
    if (!pcm || n == 0) { free(pcm); return false; }
    *out = pcm;
    *out_len = n;
    return true;
}

static void mix_cb(void *ud, Uint8 *stream, int len) {
    WSound *s = ud;
    (void)ud;
    int16_t acc[4096];
    int n = len / 2;
    if (n > 4096) n = 4096;
    memset(acc, 0, (size_t)n * 2);
    for (int v = 0; v < MAX_VOICES; v++) {
        Voice *vo = &s->voice[v];
        if (!vo->active || !vo->s) continue;
        for (int i = 0; i < n; i++) {
            if (vo->pos >= vo->s->len) { vo->active = 0; break; }
            acc[i] += (int16_t)((int)vo->s->pcm[vo->pos++] - 128) << 6;
        }
    }
    /* engine loop */
    if (s->eng.on && s->eng.smp.pcm && s->eng.smp.len) {
        for (int i = 0; i < n; i++) {
            uint32_t idx = s->eng.pos_fp >> 16;
            if (idx >= s->eng.smp.len) { s->eng.pos_fp = 0; idx = 0; }
            acc[i] += (int16_t)((int)s->eng.smp.pcm[idx] - 128) << 5;
            s->eng.pos_fp += s->eng.step_fp;
        }
    }

    int16_t *dst = (int16_t *)stream;
    for (int i = 0; i < n; i++) {
        int v = acc[i];
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        dst[i] = (int16_t)v;
    }
    if (len > n * 2) memset(stream + n * 2, 0, (size_t)(len - n * 2));
}

WSound *wsound_create(const WDat *dat) {
    WSound *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    SDL_AudioSpec want;
    SDL_zero(want);
    want.freq = 22050;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = 1024;
    want.callback = mix_cb;
    want.userdata = s;
    if (SDL_Init(SDL_INIT_AUDIO) != 0) { free(s); return NULL; }
    s->dev = SDL_OpenAudioDevice(NULL, 0, &want, &s->spec, 0);
    if (!s->dev) { free(s); return NULL; }
    s->rate = s->spec.freq;

    char name[24];
    for (int i = 0; i < WSND_COUNT; i++) {
        snprintf(name, sizeof name, "%s.VOC", SOUND_NAME[i]);
        uint32_t len;
        const uint8_t *d = wdat_find(dat, name, &len);
        if (!d) continue;
        uint8_t *pcm;
        uint32_t n;
        int rate;
        if (!voc_decode(d, len, &pcm, &n, &rate)) continue;
        /* nearest-neighbour resample to the device rate */
        if (rate != s->rate && rate > 0) {
            uint32_t on = (uint32_t)((uint64_t)n * s->rate / rate);
            uint8_t *o = malloc(on ? on : 1);
            if (o) {
                for (uint32_t k = 0; k < on; k++)
                    o[k] = pcm[(uint64_t)k * rate / s->rate];
                free(pcm);
                pcm = o;
                n = on;
            }
        }
        s->smp[i].pcm = pcm;
        s->smp[i].len = n;
    }
    /* engine sample is loaded by name, outside the id table */
    {
        uint32_t len;
        const uint8_t *d = wdat_find(dat, "MOTOR.VOC", &len);
        uint8_t *pcm; uint32_t n; int rate;
        if (d && voc_decode(d, len, &pcm, &n, &rate)) {
            if (rate != s->rate && rate > 0) {
                uint32_t on2 = (uint32_t)((uint64_t)n * s->rate / rate);
                uint8_t *o = malloc(on2 ? on2 : 1);
                if (o) {
                    for (uint32_t k = 0; k < on2; k++)
                        o[k] = pcm[(uint64_t)k * rate / s->rate];
                    free(pcm); pcm = o; n = on2;
                }
            }
            s->eng.smp.pcm = pcm;
            s->eng.smp.len = n;
            s->eng.step_fp = 1 << 16;
        }
    }

    G = s;
    SDL_PauseAudioDevice(s->dev, 0);
    return s;
}

void wsound_free(WSound *s) {
    if (!s) return;
    SDL_CloseAudioDevice(s->dev);
    for (int i = 0; i < WSND_COUNT; i++) free(s->smp[i].pcm);
    free(s->eng.smp.pcm);
    if (G == s) G = NULL;
    free(s);
}

void wsound_play(int id) {
    WSound *s = G;
    if (!s || id < 0 || id >= WSND_COUNT || !s->smp[id].pcm) return;
    SDL_LockAudioDevice(s->dev);
    int slot = -1;
    for (int v = 0; v < MAX_VOICES; v++)
        if (!s->voice[v].active) { slot = v; break; }
    if (slot < 0) slot = 0;              /* steal the oldest slot */
    s->voice[slot].s = &s->smp[id];
    s->voice[slot].pos = 0;
    s->voice[slot].active = 1;
    SDL_UnlockAudioDevice(s->dev);
}

/* engine loop: on/off plus a pitch that tracks the velocity index, so the
 * motor rises and falls with the throttle */
void wsound_engine(int on, int throttle) {
    WSound *s = G;
    if (!s || !s->eng.smp.pcm) return;
    SDL_LockAudioDevice(s->dev);
    s->eng.on = on;
    if (throttle < 0) throttle = 0;
    if (throttle > 100) throttle = 100;
    /* idle at 0.75x, full throttle at ~1.6x */
    s->eng.step_fp = (uint32_t)((0.75 + throttle * 0.0085) * 65536.0);
    SDL_UnlockAudioDevice(s->dev);
}
