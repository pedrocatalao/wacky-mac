/* Sound effects — Creative Voice (.VOC) samples mixed through SDL audio.
 *
 * The id -> sample mapping is the game's own: a 27-entry table of 12-byte
 * name slots in WW.EXE's data segment, indexed directly by the id passed to
 * the play routine (FUN_00011c04). ids 0..7 are the character voices, which
 * is why the code plays "the driver's index" as a sound id.
 */
#include "wacky.h"

#include "klm.h"
#include <SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_VOICES 8

/* The driver's play routine hands the whole VOC file to the device layer,
 * which parses the block header and takes the sample rate from its time
 * constant (FUN_0004946a: 256000000 / (0x10000 - tc*0x100)) - so every
 * effect plays at its own VOC rate, pitched by the cents parameter. */

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
    WKlm *music;
    uint8_t *song;          /* owned copy of the current KLM stream */
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
    int32_t acc[4096];
    int n = len / 4;              /* stereo S16 frames */
    if (n > 4096) n = 4096;
    memset(acc, 0, (size_t)n * 4);
    for (int v = 0; v < MAX_VOICES; v++) {
        Voice *vo = &s->voice[v];
        if (!vo->active || !vo->s) continue;
        for (int i = 0; i < n; i++) {
            if (vo->pos >= vo->s->len) { vo->active = 0; break; }
            acc[i] += (int16_t)((int)vo->s->pcm[vo->pos++] - 128) << 7;
        }
    }
    /* engine loop */
    if (s->eng.on && s->eng.smp.pcm && s->eng.smp.len) {
        for (int i = 0; i < n; i++) {
            uint32_t idx = s->eng.pos_fp >> 16;
            if (idx >= s->eng.smp.len) { s->eng.pos_fp = 0; idx = 0; }
            acc[i] += (int16_t)((int)s->eng.smp.pcm[idx] - 128) << 6;
            s->eng.pos_fp += s->eng.step_fp;
        }
    }
    /* music */
    if (s->music) {
        int16_t mus[4096];
        wklm_render(s->music, mus, n);
        for (int i = 0; i < n; i++) acc[i] += mus[i];
    }

    int16_t *dst = (int16_t *)stream;
    for (int i = 0; i < n; i++) {
        /* master gain with a soft knee: bursts compress instead of crack */
        int v = acc[i] * 7 / 4;
        if (v > 24576) v = 24576 + (v - 24576) / 4;
        else if (v < -24576) v = -24576 + (v + 24576) / 4;
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        dst[i * 2] = (int16_t)v;      /* same signal on both channels, */
        dst[i * 2 + 1] = (int16_t)v;  /* like the original's mono card */
    }
    if (len > n * 4) memset(stream + n * 4, 0, (size_t)(len - n * 4));
    /* diagnostic: dump the mix (mono) when WW_MIXDUMP names a file */
    {
        static FILE *dumpf;
        static int tried;
        if (!tried) {
            tried = 1;
            const char *p = getenv("WW_MIXDUMP");
            if (p) dumpf = fopen(p, "wb");
        }
        if (dumpf)
            for (int i = 0; i < n; i++) fwrite(&dst[i * 2], 2, 1, dumpf);
    }
}

WSound *wsound_create(const WDat *dat) {
    WSound *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    SDL_AudioSpec want;
    SDL_zero(want);
    want.freq = 22050;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
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
        /* nearest-neighbour resample from the VOC's own rate to the device */
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
    wklm_free(s->music);
    free(s->song);
    if (G == s) G = NULL;
    free(s);
}

/* start a KLM song by base name ("TURBO"); replaces any playing song */
static void music_start(const WDat *dat, const char *base, int loop) {
    WSound *s = G;
    if (!s) return;
    char name[24];
    snprintf(name, sizeof name, "%s.KLM", base);
    uint32_t len;
    const uint8_t *d = wdat_find(dat, name, &len);
    if (!d) return;
    uint8_t *copy = malloc(len);
    if (!copy) return;
    memcpy(copy, d, len);
    SDL_LockAudioDevice(s->dev);
    if (!s->music) s->music = wklm_create(s->rate);
    if (s->music) {
        uint8_t *old = s->song;
        s->song = copy;
        if (!wklm_start(s->music, s->song, len, loop)) {
            free(s->song);
            s->song = old;
        } else {
            free(old);
        }
    } else {
        free(copy);
    }
    SDL_UnlockAudioDevice(s->dev);
}

void wsound_music(const WDat *dat, const char *base) { music_start(dat, base, 1); }
void wsound_music_once(const WDat *dat, const char *base) { music_start(dat, base, 0); }

int wsound_music_playing(void) {
    WSound *s = G;
    return s && s->music && wklm_playing(s->music);
}

void wsound_music_stop(void) {
    WSound *s = G;
    if (!s || !s->music) return;
    SDL_LockAudioDevice(s->dev);
    wklm_stop(s->music);
    SDL_UnlockAudioDevice(s->dev);
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

/* engine loop pitch, the original's model: the race tick programs the motor
 * voice with a pitch of (shown_speed * 20 - 1800) cents (FUN_00041b44 call
 * site), which the driver maps through its 2^(cents/1200) table
 * (FUN_0004f140). shown_speed is the slewed speedometer value, so the motor
 * idles 1.5 octaves below the sample and climbs with the needle. */
void wsound_engine(int on, int shown_speed) {
    WSound *s = G;
    if (!s || !s->eng.smp.pcm) return;
    SDL_LockAudioDevice(s->dev);
    s->eng.on = on;
    if (shown_speed < 0) shown_speed = 0;
    if (shown_speed > 130) shown_speed = 130;
    double ratio = exp2((shown_speed * 20.0 - 1800.0) / 1200.0);
    s->eng.step_fp = (uint32_t)(ratio * 65536.0);
    SDL_UnlockAudioDevice(s->dev);
}
