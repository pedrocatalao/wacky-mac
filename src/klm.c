/* KLM music player — transcription of the AdLib music driver in WW.EXE.
 *
 * File layout: {u16 tempo (ticks/sec), u8 mode, u16 song_offset}, then
 * 11-byte instrument records up to song_offset, then the command stream:
 *   0x0n        note off, channel n
 *   0x1n ...    note on, channel n; operands are the raw A0/B0 register
 *               values: 2 bytes for channels 0-6, 4 for channel 8 (tom-tom
 *               also programs channel 7's frequency), none for 7/9/10
 *   0x2n vv     channel volume 0..127
 *   0x3n ii     select instrument ii for channel n
 *   0x4n        key the previous note again
 *   0xFD tt     delay tt ticks
 *   0xFE tttt   delay tttt ticks (little-endian)
 *   0xFF        end of song (loops)
 *
 * Channels 0-5 are melodic; 6-10 are the OPL rhythm section (bass, snare,
 * tom, cymbal, hi-hat), enabled once at init exactly like the driver does.
 *
 * Instrument record: [mod 40, car 40, mod 60, car 60, mod 80, car 80,
 *                     mod 20, car 20, mod E0, car E0, C0].
 */
#include "klm.h"

#include "opl2.h"
#include <stdlib.h>
#include <string.h>

#define KCH 11

/* operator index pairs per driver channel (DAT_00064588): melodic
 * modulator/carrier, single slot for the percussion voices */
static const int OPS_OF[KCH][2] = {
    {0,3},{1,4},{2,5},{6,9},{7,10},{8,11},   /* melodic 0-5 */
    {12,15},                                 /* 6 bass drum  */
    {16,-1},                                 /* 7 snare      */
    {14,-1},                                 /* 8 tom-tom    */
    {17,-1},                                 /* 9 cymbal     */
    {13,-1},                                 /* 10 hi-hat    */
};
/* per-operator register offset (DAT_000645e0) */
static const int OPREG[18] = {0,1,2,3,4,5,8,9,0xA,0xB,0xC,0xD,
                              0x10,0x11,0x12,0x13,0x14,0x15};
/* rhythm key bit in register 0xBD per channel (DAT_0006455c) */
static const int BDBIT[KCH] = {0,0,0,0,0,0,0x10,0x08,0x04,0x02,0x01};

#define OPL_RATE 49716    /* the chip's native sample rate */

struct WKlm {
    WOpl *opl;
    int rate;                       /* output rate */
    const uint8_t *data;
    uint32_t len, songoff, pos;
    int tempo, wait, playing;
    /* sequencer clock and output resampler, both 16.16 fixed point */
    int64_t  tick_rem_fp;           /* chip samples until the next tick */
    uint32_t tick_len_fp;
    uint32_t rs_frac, rs_step;
    int16_t  rs_s0, rs_s1;          /* newest / previous chip samples */
    /* driver state */
    int gvol;                       /* DAT_00064570 master music volume */
    int opvol[18];                  /* DAT_0009154c scaled per-op volume */
    int maxlvl[18];                 /* DAT_00091594 instrument loudness  */
    int kslbits[18];                /* DAT_000915dc KSL bits of reg 0x40 */
    uint8_t lastA0[9], lastB0[9];   /* DAT_0009150c / DAT_000914cc      */
    int bd;                         /* DAT_0009167c rhythm key state    */
    const uint8_t *instr[KCH];      /* DAT_00091624 current instrument  */
    void   (*log_fn)(void *ud, uint8_t reg, uint8_t val);
    void    *log_ud;
};

void wklm_set_logger(WKlm *k, void (*fn)(void *, uint8_t, uint8_t), void *ud) {
    if (k) { k->log_fn = fn; k->log_ud = ud; }
}

static void oplw(WKlm *k, uint8_t reg, uint8_t val) {
    if (k->log_fn) k->log_fn(k->log_ud, reg, val);
    wopl_write(k->opl, reg, val);
}

/* FUN_00050280: write the level register of one operator from its
 * instrument loudness scaled by the channel volume */
static void op_level(WKlm *k, int op) {
    int v = 0x3F - (k->maxlvl[op] * k->opvol[op] * 2 + 0x7F) / 0xFE;
    oplw(k, (uint8_t)(OPREG[op] + 0x40), (uint8_t)(v | k->kslbits[op]));
}

/* FUN_00050374: OPL init — wave select on, channels cleared, default
 * snare/hi-hat frequencies, rhythm mode enabled */
static void drv_init(WKlm *k) {
    for (int op = 0; op < 18; op++) k->opvol[op] = k->gvol * 0x7F / 0xFF;
    oplw(k, 1, 0x20);
    oplw(k, 8, 0);
    for (int c = 0; c < 9; c++) {
        oplw(k, (uint8_t)(0xA0 + c), 0);
        oplw(k, (uint8_t)(0xB0 + c), 0);
        k->lastA0[c] = k->lastB0[c] = 0;
    }
    for (int c = 0; c < KCH; c++) k->instr[c] = NULL;
    for (int op = 0; op < 18; op++) oplw(k, (uint8_t)(OPREG[op] + 0xE0), 0);
    oplw(k, 0xA8, 0x57);
    oplw(k, 0xB8, 9);
    oplw(k, 0xA7, 3);
    oplw(k, 0xB7, 10);
    k->bd = 0x20;
    oplw(k, 0xBD, (uint8_t)k->bd);
}

/* FUN_00050478: load an 11-byte instrument into a channel */
static void drv_instrument(WKlm *k, int ch, const uint8_t *p) {
    if (p == k->instr[ch]) return;
    k->instr[ch] = p;
    int op1 = OPS_OF[ch][0];
    k->maxlvl[op1] = 0x3F - (p[0] & 0x3F);
    k->kslbits[op1] = p[0] & 0xC0;
    oplw(k, (uint8_t)(OPREG[op1] + 0x60), p[2]);
    oplw(k, (uint8_t)(OPREG[op1] + 0x80), p[4]);
    oplw(k, (uint8_t)(OPREG[op1] + 0x20), p[6]);
    oplw(k, (uint8_t)(OPREG[op1] + 0xE0), p[8]);
    if (ch < 7) {
        oplw(k, (uint8_t)(OPREG[op1] + 0x40), p[0]);
        oplw(k, (uint8_t)(0xC0 + ch), p[10]);
        int op2 = OPS_OF[ch][1];
        k->maxlvl[op2] = 0x3F - (p[1] & 0x3F);
        k->kslbits[op2] = p[1] & 0xC0;
        op_level(k, op2);
        oplw(k, (uint8_t)(OPREG[op2] + 0x60), p[3]);
        oplw(k, (uint8_t)(OPREG[op2] + 0x80), p[5]);
        oplw(k, (uint8_t)(OPREG[op2] + 0x20), p[7]);
        oplw(k, (uint8_t)(OPREG[op2] + 0xE0), p[9]);
    } else {
        op_level(k, op1);
    }
}

/* FUN_000505d0: channel volume; additive instruments scale both slots */
static void drv_volume(WKlm *k, int ch, int vol) {
    int v = k->gvol * vol / 0xFF;
    if (ch < 7) {
        int car = OPS_OF[ch][1];
        k->opvol[car] = v;
        op_level(k, car);
        if (!k->instr[ch] || (k->instr[ch][10] & 1) == 0) return;
        int mod = OPS_OF[ch][0];
        k->opvol[mod] = v;
        op_level(k, mod);
    } else {
        int op = OPS_OF[ch][0];
        k->opvol[op] = v;
        op_level(k, op);
    }
}

/* FUN_00050660: note on; returns how many operand bytes were consumed */
static int drv_note_on(WKlm *k, int ch, const uint8_t *p) {
    if (ch < 6) {
        oplw(k, (uint8_t)(0xA0 + ch), p[0]); k->lastA0[ch] = p[0];
        oplw(k, (uint8_t)(0xB0 + ch), p[1]); k->lastB0[ch] = p[1];
        return 2;
    }
    int used = 0;
    if (ch == 6) {
        oplw(k, 0xA6, p[0]); k->lastA0[6] = p[0];
        oplw(k, 0xB6, p[1]); k->lastB0[6] = p[1];
        used = 2;
    } else if (ch == 8) {
        oplw(k, 0xA8, p[0]); k->lastA0[8] = p[0];
        oplw(k, 0xB8, p[1]); k->lastB0[8] = p[1];
        oplw(k, 0xA7, p[2]); k->lastA0[7] = p[2];
        oplw(k, 0xB7, p[3]); k->lastB0[7] = p[3];
        used = 4;
    }
    k->bd |= BDBIT[ch];
    oplw(k, 0xBD, (uint8_t)k->bd);
    return used;
}

/* FUN_0005074c: note off */
static void drv_note_off(WKlm *k, int ch) {
    if (ch < 6) {
        oplw(k, (uint8_t)(0xB0 + ch), (uint8_t)(k->lastB0[ch] & 0xDF));
    } else {
        k->bd &= ~BDBIT[ch];
        oplw(k, 0xBD, (uint8_t)k->bd);
    }
}

/* FUN_00050790: key the previous note again */
static void drv_rekey(WKlm *k, int ch) {
    if (ch < 6) {
        oplw(k, (uint8_t)(0xA0 + ch), k->lastA0[ch]);
        oplw(k, (uint8_t)(0xB0 + ch), k->lastB0[ch]);
    } else {
        k->bd |= BDBIT[ch];
        oplw(k, 0xBD, (uint8_t)k->bd);
    }
}

WKlm *wklm_create(int rate) {
    WKlm *k = calloc(1, sizeof *k);
    if (!k) return NULL;
    k->opl = wopl_create(OPL_RATE);
    if (!k->opl) { free(k); return NULL; }
    k->rate = rate;
    k->gvol = 0xFF;
    return k;
}

void wklm_free(WKlm *k) {
    if (!k) return;
    wopl_free(k->opl);
    free(k);
}

void wklm_stop(WKlm *k) {
    if (!k) return;
    k->playing = 0;
    wopl_reset(k->opl);
}

bool wklm_start(WKlm *k, const uint8_t *data, uint32_t len) {
    if (!k || !data || len < 6) return false;
    uint16_t tempo = (uint16_t)(data[0] | data[1] << 8);
    uint16_t songoff = (uint16_t)(data[3] | data[4] << 8);
    if (tempo == 0 || songoff < 5 || songoff >= len) return false;
    k->data = data;
    k->len = len;
    k->songoff = songoff;
    k->pos = songoff;
    k->tempo = tempo;
    k->wait = 0;
    k->tick_len_fp = (uint32_t)(((uint64_t)OPL_RATE << 16) / tempo);
    k->tick_rem_fp = 0;
    k->rs_step = (uint32_t)(((uint64_t)OPL_RATE << 16) / k->rate);
    k->rs_frac = 0;
    k->rs_s0 = k->rs_s1 = 0;
    wopl_reset(k->opl);
    drv_init(k);
    k->playing = 1;
    return true;
}

/* run commands until the stream yields a delay */
static void seq_tick(WKlm *k) {
    if (k->wait > 0) { k->wait--; return; }
    const uint8_t *d = k->data;
    while (k->pos < k->len) {
        uint8_t c = d[k->pos];
        if (c == 0xFF) {                       /* end: loop the song */
            k->pos = k->songoff;
            continue;
        }
        if (c == 0xFD) {
            k->wait = d[k->pos + 1];
            k->pos += 2;
            break;
        }
        if (c == 0xFE) {
            k->wait = d[k->pos + 1] | d[k->pos + 2] << 8;
            k->pos += 3;
            break;
        }
        int ch = c & 0x0F, hi = c >> 4;
        if (ch >= KCH) { k->pos++; continue; }
        switch (hi) {
        case 0: drv_note_off(k, ch); k->pos += 1; break;
        case 1: k->pos += 1 + (uint32_t)drv_note_on(k, ch, d + k->pos + 1); break;
        case 2: drv_volume(k, ch, d[k->pos + 1]); k->pos += 2; break;
        case 3: {
            uint32_t rec = 5 + (uint32_t)d[k->pos + 1] * 11;
            if (rec + 11 <= k->songoff) drv_instrument(k, ch, d + rec);
            k->pos += 2;
            break;
        }
        case 4: drv_rekey(k, ch); k->pos += 1; break;
        default: k->pos += 1; break;
        }
    }
    if (k->wait > 0) k->wait--;
}

/* one sample at the chip rate, ticking the sequencer on schedule */
static int16_t chip_sample(WKlm *k) {
    if (k->tick_rem_fp <= 0) {
        seq_tick(k);
        k->tick_rem_fp += k->tick_len_fp;
    }
    k->tick_rem_fp -= 1 << 16;
    int16_t s;
    wopl_render(k->opl, &s, 1);
    return s;
}

/* linear resample 49716 Hz -> output rate */
void wklm_render(WKlm *k, int16_t *out, int n) {
    if (!k || !k->playing) { memset(out, 0, (size_t)n * 2); return; }
    for (int i = 0; i < n; i++) {
        k->rs_frac += k->rs_step;
        while (k->rs_frac >= 1 << 16) {
            k->rs_frac -= 1 << 16;
            k->rs_s1 = k->rs_s0;
            k->rs_s0 = chip_sample(k);
        }
        out[i] = (int16_t)(k->rs_s1 +
                 (((int)k->rs_s0 - k->rs_s1) * (int)k->rs_frac >> 16));
    }
}
