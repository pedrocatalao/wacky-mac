/* OPL2 (YM3812) synthesiser — a proper emulation of the chip the game's
 * KLM music was written for.
 *
 * Follows the documented hardware behaviour: the 256-entry log-sin and
 * exponential ROM tables, 10-bit phase with multiplier and vibrato, the
 * envelope generator in 0.1875 dB steps with rate key-scaling, total level
 * at 0.75 dB/step, KSL, tremolo, operator feedback averaging the last two
 * outputs, the four wave shapes, and rhythm mode with the real phase-bit
 * formulas for hi-hat, snare and cymbal plus the 23-bit noise LFSR.
 *
 * The core runs at the chip's native 49716 Hz; callers resample.
 */
#include "opl2.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define OPS 18
#define CHS 9

/* ---- ROM tables -------------------------------------------------------- */

static uint16_t logsin_t[256];  /* -log2(sin) in 1/256 log2 units */
static uint16_t exp_t[256];     /* (2^(i/256) - 1) * 1024 */
static int      tabs_done;

static void build_tables(void) {
    if (tabs_done) return;
    for (int i = 0; i < 256; i++) {
        logsin_t[i] = (uint16_t)lround(-log2(sin((i + 0.5) * M_PI / 512.0)) * 256.0);
        exp_t[i] = (uint16_t)lround((exp2((double)i / 256.0) - 1.0) * 1024.0);
    }
    tabs_done = 1;
}

/* attenuated sine lookup: phase 0..1023, att in 1/256 log2 units ("logsin
 * units"); returns the chip's 13-bit signed output */
static int op_wave(int wave, uint32_t phase, uint32_t att) {
    phase &= 1023;
    int sign = 0;
    uint32_t idx = phase & 255;
    uint32_t half = phase & 512, quarter = phase & 256;
    if (quarter) idx = 255 - idx;
    uint32_t val;
    switch (wave) {
    default:
    case 0:                                   /* full sine */
        val = logsin_t[idx];
        sign = half != 0;
        break;
    case 1:                                   /* half sine */
        if (half) return 0;
        val = logsin_t[idx];
        break;
    case 2:                                   /* absolute sine */
        val = logsin_t[idx];
        break;
    case 3:                                   /* quarter pulses */
        if (quarter) return 0;
        val = logsin_t[idx];
        break;
    }
    uint32_t total = val + att;
    if (total > 8191) total = 8191;           /* below audible floor */
    int out = ((exp_t[total & 255] + 1024) << 1) >> (total >> 8);
    return sign ? -out : out;
}

/* ---- operator / channel state ------------------------------------------ */

enum { EG_OFF, EG_ATTACK, EG_DECAY, EG_SUSTAIN, EG_RELEASE };

typedef struct {
    /* registers */
    uint8_t am, vib, egt, ksr, mult;   /* 0x20 */
    uint8_t ksl, tl;                   /* 0x40 */
    uint8_t ar, dr;                    /* 0x60 */
    uint8_t sl, rr;                    /* 0x80 */
    uint8_t wave;                      /* 0xE0 */
    /* runtime */
    uint32_t phase;                    /* 19-bit phase counter */
    int32_t  env;                      /* 0 (loud) .. 511 (silent) */
    int      stage;
    int32_t  out0, out1;               /* last two outputs (feedback) */
    int      ksl_att;                  /* cached KSL attenuation, env units */
    int      ksv;                      /* cached key-scale value for rates */
} Op;

typedef struct {
    uint16_t fnum;
    uint8_t  block, keyon, fb, cnt;
} Ch;

struct WOpl {
    Op  op[OPS];
    Ch  ch[CHS];
    int rate;                          /* informational; core is 49716 Hz */
    uint8_t  bd;
    uint32_t noise;                    /* 23-bit LFSR */
    uint32_t eg_cnt;                   /* envelope timer */
    uint32_t lfo_am_cnt, lfo_vib_cnt;
    uint8_t  dam, dvb;                 /* depth bits from 0xBD */
    int      tremolo;                  /* current AM attenuation, env units */
    int      vib_step;
};

/* channel -> operator pair (the chip's slot layout) */
static const int OPMAP[CHS][2] = {
    {0,3},{1,4},{2,5},{6,9},{7,10},{8,11},{12,15},{13,16},{14,17}
};
static const int REG2OP[32] = {
    0,1,2,3,4,5,-1,-1, 6,7,8,9,10,11,-1,-1, 12,13,14,15,16,17,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1
};
/* operator -> owning channel, for frequency-derived values */
static const int OP2CH[OPS] = {0,1,2,0,1,2,3,4,5,3,4,5,6,7,8,6,7,8};

/* KSL base attenuation per fnum>>6, in 0.75 dB units (hardware table) */
static const int KSL_BASE[16] = {0,24,32,37,40,43,45,47,48,50,51,52,53,54,55,56};

WOpl *wopl_create(int rate) {
    build_tables();
    WOpl *o = calloc(1, sizeof *o);
    if (o) { o->rate = rate; o->noise = 1; }
    return o;
}

void wopl_free(WOpl *o) { free(o); }

void wopl_reset(WOpl *o) {
    if (!o) return;
    int r = o->rate;
    memset(o, 0, sizeof *o);
    o->rate = r;
    o->noise = 1;
    for (int i = 0; i < OPS; i++) { o->op[i].env = 511; o->op[i].stage = EG_OFF; }
}

/* refresh an operator's cached frequency-derived values */
static void op_refresh(WOpl *o, int oi) {
    Op *p = &o->op[oi];
    const Ch *c = &o->ch[OP2CH[oi]];
    int ks = (c->block << 1) | (c->fnum >> 9);
    p->ksv = p->ksr ? ks : ks >> 2;
    int ksl = KSL_BASE[c->fnum >> 6] - 8 * (7 - c->block);
    if (ksl < 0) ksl = 0;
    /* ksl bits: off, 3 dB/oct, 1.5 dB/oct, 6 dB/oct -> shift 3/1/2/0 on the
     * 0.75 dB base gives env units (0.1875 dB) after <<2 */
    static const int KSL_SHIFT[4] = {31, 1, 2, 0};
    p->ksl_att = p->ksl ? (ksl << 2) >> KSL_SHIFT[p->ksl] : 0;
}

static void op_key_on(WOpl *o, int oi) {
    Op *p = &o->op[oi];
    if (p->stage == EG_OFF || p->stage == EG_RELEASE) {
        p->phase = 0;
        p->stage = EG_ATTACK;
        if (p->ar >= 15) { p->env = 0; p->stage = EG_DECAY; }
    }
}

static void op_key_off(WOpl *o, int oi) {
    Op *p = &o->op[oi];
    if (p->stage != EG_OFF) p->stage = EG_RELEASE;
}

void wopl_write(WOpl *o, uint8_t reg, uint8_t val) {
    if (!o) return;
    if (reg == 0xBD) {
        uint8_t was = o->bd;
        o->bd = val;
        o->dam = (val >> 7) & 1;
        o->dvb = (val >> 6) & 1;
        if (!(val & 0x20)) return;
        uint8_t rise = (uint8_t)(val & ~was), fall = (uint8_t)(was & ~val);
        static const struct { uint8_t bit; int op1, op2; } K[5] = {
            {0x10, 12, 15}, {0x08, 16, -1}, {0x04, 14, -1},
            {0x02, 17, -1}, {0x01, 13, -1},
        };
        for (int i = 0; i < 5; i++) {
            if (rise & K[i].bit) {
                op_key_on(o, K[i].op1);
                if (K[i].op2 >= 0) op_key_on(o, K[i].op2);
            } else if (fall & K[i].bit) {
                op_key_off(o, K[i].op1);
                if (K[i].op2 >= 0) op_key_off(o, K[i].op2);
            }
        }
        return;
    }
    int grp = reg & 0xE0, idx = reg & 0x1F;
    if (grp == 0x20 || grp == 0x40 || grp == 0x60 || grp == 0x80 || grp == 0xE0) {
        int oi = idx < 32 ? REG2OP[idx] : -1;
        if (oi < 0) return;
        Op *p = &o->op[oi];
        switch (grp) {
        case 0x20:
            p->am = val >> 7; p->vib = (val >> 6) & 1; p->egt = (val >> 5) & 1;
            p->ksr = (val >> 4) & 1; p->mult = val & 15;
            op_refresh(o, oi);
            break;
        case 0x40: p->ksl = val >> 6; p->tl = val & 63; op_refresh(o, oi); break;
        case 0x60: p->ar = val >> 4; p->dr = val & 15; break;
        case 0x80: p->sl = val >> 4; p->rr = val & 15; break;
        case 0xE0: p->wave = val & 3; break;
        }
        return;
    }
    if (reg >= 0xA0 && reg <= 0xA8) {
        Ch *c = &o->ch[reg - 0xA0];
        c->fnum = (uint16_t)((c->fnum & 0x300) | val);
        op_refresh(o, OPMAP[reg - 0xA0][0]);
        op_refresh(o, OPMAP[reg - 0xA0][1]);
    } else if (reg >= 0xB0 && reg <= 0xB8) {
        int chn = reg - 0xB0;
        Ch *c = &o->ch[chn];
        c->fnum = (uint16_t)((c->fnum & 0xFF) | ((val & 3) << 8));
        c->block = (val >> 2) & 7;
        op_refresh(o, OPMAP[chn][0]);
        op_refresh(o, OPMAP[chn][1]);
        /* rhythm mode owns the keying of channels 6-8 */
        if ((o->bd & 0x20) && chn >= 6) return;
        int on = (val >> 5) & 1;
        if (on && !c->keyon) {
            op_key_on(o, OPMAP[chn][0]);
            op_key_on(o, OPMAP[chn][1]);
        } else if (!on && c->keyon) {
            op_key_off(o, OPMAP[chn][0]);
            op_key_off(o, OPMAP[chn][1]);
        }
        c->keyon = (uint8_t)on;
    } else if (reg >= 0xC0 && reg <= 0xC8) {
        Ch *c = &o->ch[reg - 0xC0];
        c->fb = (val >> 1) & 7;
        c->cnt = val & 1;
    }
}

/* ---- envelope generator ------------------------------------------------ */

/* per-step increment patterns for the two low rate bits */
static const int EG_PAT[4][8] = {
    {0,1,0,1,0,1,0,1}, {0,1,0,1,1,1,0,1}, {0,1,1,1,0,1,1,1}, {0,1,1,1,1,1,1,1},
};

/* advance one operator's envelope; called once per sample */
static void env_step(WOpl *o, Op *p) {
    int r4, target;
    switch (p->stage) {
    case EG_ATTACK:  r4 = p->ar; break;
    case EG_DECAY:   r4 = p->dr; break;
    case EG_SUSTAIN:
        if (p->egt) return;                    /* held until key-off */
        r4 = p->rr;
        break;
    case EG_RELEASE: r4 = p->rr; break;
    default: return;
    }
    if (r4 == 0) return;
    int rate = r4 * 4 + p->ksv;
    if (rate > 63) rate = 63;
    int shift = 12 - (rate >> 2);
    int inc;
    if (shift > 0) {
        if (o->eg_cnt & ((1u << shift) - 1)) return;
        inc = EG_PAT[rate & 3][(o->eg_cnt >> shift) & 7];
    } else {
        /* rates 48+: one to eight units every sample */
        inc = (1 << (-shift)) + EG_PAT[rate & 3][o->eg_cnt & 7] * (1 << (-shift) >> 1);
        if (inc < 1) inc = 1;
    }
    if (p->stage == EG_ATTACK) {
        if (rate >= 60) { p->env = 0; }
        else p->env += (~p->env * inc) >> 3;
        if (p->env <= 0) { p->env = 0; p->stage = EG_DECAY; }
        return;
    }
    p->env += inc;
    if (p->env > 511) p->env = 511;
    if (p->stage == EG_DECAY) {
        target = p->sl == 15 ? 511 : p->sl << 4;   /* 3 dB per SL step */
        if (p->env >= target) { p->env = target; p->stage = EG_SUSTAIN; }
    } else if (p->stage == EG_RELEASE && p->env >= 511) {
        p->stage = EG_OFF;
    }
}

/* total attenuation for output, in logsin units (1/256 log2) */
static uint32_t op_att(const WOpl *o, const Op *p) {
    int env = p->env + (p->tl << 2) + p->ksl_att;
    if (p->am) env += o->dam ? o->tremolo : o->tremolo >> 2;
    if (env > 511) env = 511;
    return (uint32_t)env << 3;
}

/* phase increment with vibrato (applies to the fnum high bits) */
static uint32_t op_phase_inc(const WOpl *o, const Op *p, const Ch *c) {
    static const int MULT2[16] = {1,2,4,6,8,10,12,14,16,18,20,20,24,24,30,30};
    int fnum = c->fnum;
    if (p->vib) {
        int fh = fnum >> 7;
        int step = o->vib_step;
        int delta = (step & 3) == 0 ? 0 : (step & 1 ? fh >> 1 : fh);
        if (!o->dvb) delta >>= 1;
        fnum += (step & 4) ? -delta : delta;
    }
    return ((uint32_t)fnum * MULT2[p->mult] << c->block) >> 2;
}

/* run one operator; pm is the 10-bit-domain phase modulation input */
static int op_run(WOpl *o, Op *p, const Ch *c, int pm) {
    env_step(o, p);
    p->phase = (p->phase + op_phase_inc(o, p, c)) & 0x7FFFF;
    if (p->stage == EG_OFF) return 0;
    return op_wave(p->wave, (p->phase >> 9) + (uint32_t)pm, op_att(o, p));
}

/* rhythm voices: the chip derives their waveform phases from the raw phase
 * bits of operators 13 (hi-hat slot) and 17 (cymbal slot) plus noise */
static uint32_t rhythm_phase(WOpl *o, int voice) {
    uint32_t ph13 = o->op[13].phase >> 9, ph17 = o->op[17].phase >> 9;
    int noise = (int)(o->noise & 1);
    int res1 = ((ph13 >> 2 ^ ph13 >> 7) & 1) | ((ph13 >> 3) & 1);
    int res2 = ((ph17 >> 3 ^ ph17 >> 5) & 1);
    switch (voice) {
    case 0: {                                   /* hi-hat */
        uint32_t phase = res1 ? (0x200 | (0xD0 >> 2)) : 0xD0;
        if (res2) phase = 0x200 | (0xD0 >> 2);
        if (phase & 0x200) { if (noise) phase = 0x200 | 0xD0; }
        else               { if (noise) phase = 0xD0 >> 2; }
        return phase;
    }
    case 1: {                                   /* snare */
        uint32_t phase = ((ph13 >> 8) & 1) ? 0x200 : 0x100;
        if (noise) phase ^= 0x100;
        return phase;
    }
    default:                                    /* cymbal */
        return (res1 | res2) ? 0x300 : 0x100;
    }
}

void wopl_render(WOpl *o, int16_t *out, int n) {
    if (!o) { memset(out, 0, (size_t)n * 2); return; }
    int rhythm = o->bd & 0x20;
    for (int i = 0; i < n; i++) {
        o->eg_cnt++;
        /* tremolo: 210-sample steps over a 27-step triangle (~3.7 Hz),
         * 0..26 envelope units = 4.8 dB */
        if (++o->lfo_am_cnt >= 210 * 54) o->lfo_am_cnt = 0;
        int am_step = (int)(o->lfo_am_cnt / 210);
        o->tremolo = am_step < 27 ? am_step : 53 - am_step;
        /* vibrato: 1024-sample steps through 8 positions (~6.1 Hz) */
        if (++o->lfo_vib_cnt >= 1024 * 8) o->lfo_vib_cnt = 0;
        o->vib_step = (int)(o->lfo_vib_cnt >> 10);
        /* noise LFSR, clocked every sample */
        uint32_t nbit = ((o->noise >> 14) ^ o->noise) & 1;
        o->noise = (o->noise >> 1) | (nbit << 22);

        int acc = 0;
        int melodic = rhythm ? 6 : CHS;
        for (int c = 0; c < melodic; c++) {
            Ch *ch = &o->ch[c];
            Op *m = &o->op[OPMAP[c][0]], *k = &o->op[OPMAP[c][1]];
            if (m->stage == EG_OFF && k->stage == EG_OFF) continue;
            int fb = ch->fb ? (int)((m->out0 + m->out1) >> (9 - ch->fb)) : 0;
            int mo = op_run(o, m, ch, fb);
            m->out1 = m->out0;
            m->out0 = mo;
            if (ch->cnt) acc += mo + op_run(o, k, ch, 0);
            else         acc += op_run(o, k, ch, mo);
        }
        if (rhythm) {
            /* bass drum: normal 2-op FM on channel 6, double weight */
            Ch *c6 = &o->ch[6];
            Op *m = &o->op[12], *k = &o->op[15];
            if (m->stage != EG_OFF || k->stage != EG_OFF) {
                int fb = c6->fb ? (int)((m->out0 + m->out1) >> (9 - c6->fb)) : 0;
                int mo = op_run(o, m, c6, fb);
                m->out1 = m->out0;
                m->out0 = mo;
                acc += (c6->cnt ? op_run(o, k, c6, 0) : op_run(o, k, c6, mo)) * 2;
            }
            /* hi-hat, snare, tom, cymbal — phases per the chip's formulas */
            Op *hh = &o->op[13], *sd = &o->op[16];
            Op *tt = &o->op[14], *cy = &o->op[17];
            if (hh->stage != EG_OFF) {
                env_step(o, hh);
                hh->phase = (hh->phase + op_phase_inc(o, hh, &o->ch[7])) & 0x7FFFF;
                if (hh->stage != EG_OFF)
                    acc += op_wave(hh->wave, rhythm_phase(o, 0), op_att(o, hh)) * 2;
            }
            if (sd->stage != EG_OFF) {
                env_step(o, sd);
                sd->phase = (sd->phase + op_phase_inc(o, sd, &o->ch[7])) & 0x7FFFF;
                if (sd->stage != EG_OFF)
                    acc += op_wave(sd->wave, rhythm_phase(o, 1), op_att(o, sd)) * 2;
            }
            if (tt->stage != EG_OFF)
                acc += op_run(o, tt, &o->ch[8], 0) * 2;
            if (cy->stage != EG_OFF) {
                env_step(o, cy);
                cy->phase = (cy->phase + op_phase_inc(o, cy, &o->ch[8])) & 0x7FFFF;
                if (cy->stage != EG_OFF)
                    acc += op_wave(cy->wave, rhythm_phase(o, 2), op_att(o, cy)) * 2;
            }
        }
        if (acc > 32767) acc = 32767;
        if (acc < -32768) acc = -32768;
        out[i] = (int16_t)acc;
    }
}
