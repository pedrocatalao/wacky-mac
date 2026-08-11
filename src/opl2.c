/* Compact OPL2 (YM3812) synthesiser — enough of the chip to play the game's
 * AdLib music: 9 two-operator channels, the four wave shapes, ADSR envelopes
 * with key-scaling, FM or additive connection, and operator feedback.
 *
 * Written against the published YM3812 register map so the KLM streams (which
 * are literally register writes) drive it the way the original hardware was
 * driven. Exponential/log tables follow the chip's 256-entry scheme.
 */
#include "opl2.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define OPS 18
#define CHS 9

static int16_t sintab[4][1024];
static int     tabs_done;

static void build_tables(void) {
    if (tabs_done) return;
    for (int i = 0; i < 1024; i++) {
        double s = sin((i + 0.5) * 2.0 * M_PI / 1024.0);
        sintab[0][i] = (int16_t)(s * 4084.0);                    /* full sine   */
        sintab[1][i] = i < 512 ? sintab[0][i] : 0;               /* half sine   */
        sintab[2][i] = sintab[0][i & 511] > 0 ? sintab[0][i & 511]
                                              : (int16_t)(-sintab[0][i & 511]);
        sintab[3][i] = (i & 511) < 256 ? sintab[0][i & 255] : 0; /* pulse sine  */
    }
    tabs_done = 1;
}

typedef struct {
    uint8_t am, vib, egt, ksr, mult;   /* 0x20 */
    uint8_t ksl, tl;                   /* 0x40 */
    uint8_t ar, dr;                    /* 0x60 */
    uint8_t sl, rr;                    /* 0x80 */
    uint8_t wave;                      /* 0xE0 */
    /* runtime */
    uint32_t phase;
    double   env;                      /* 0..1 linear amplitude */
    int      stage;                    /* 0 off, 1 attack, 2 decay, 3 sustain, 4 release */
    int16_t  out, prev;
} Op;

typedef struct {
    uint16_t fnum;
    uint8_t  block, keyon, fb, cnt;
} Ch;

struct WOpl {
    Op  op[OPS];
    Ch  ch[CHS];
    int rate;
    uint8_t  bd;      /* register 0xBD: rhythm enable + key bits  */
    uint32_t lfsr;    /* noise generator for snare/hihat/cymbal   */
};

/* operator index for (channel, slot) — the chip's irregular layout */
static const int OPMAP[CHS][2] = {
    {0,3},{1,4},{2,5},{6,9},{7,10},{8,11},{12,15},{13,16},{14,17}
};
/* register offset -> operator */
static const int REG2OP[32] = {
    0,1,2,3,4,5,-1,-1, 6,7,8,9,10,11,-1,-1, 12,13,14,15,16,17,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1
};

WOpl *wopl_create(int rate) {
    build_tables();
    WOpl *o = calloc(1, sizeof *o);
    if (o) { o->rate = rate; o->lfsr = 1; }
    return o;
}

void wopl_free(WOpl *o) { free(o); }

void wopl_reset(WOpl *o) {
    if (!o) return;
    int r = o->rate;
    memset(o, 0, sizeof *o);
    o->rate = r;
    o->lfsr = 1;
}

/* attack/decay/release rates: the chip's rate index -> per-sample increment */
static double env_rate(int rate_idx, int srate, int attack) {
    if (rate_idx == 0) return 0.0;
    /* the chip's envelope times roughly halve per rate step; rate 15 is
     * near-instant, rate 1 is seconds long */
    double ms = (attack ? 2400.0 : 9600.0) / pow(2.0, rate_idx / 2.0);
    if (ms < 0.2) ms = 0.2;
    return 1000.0 / (ms * srate);
}

static void op_key(Op *p, int on) {
    if (on) { p->stage = 1; p->phase = 0; }
    else if (p->stage != 0) p->stage = 4;
}

void wopl_write(WOpl *o, uint8_t reg, uint8_t val) {
    if (!o) return;
    if (reg == 0xBD) {
        /* rhythm mode: bit5 enable, bits 4..0 key BD/SD/TT/CY/HH */
        uint8_t was = o->bd;
        o->bd = val;
        uint8_t rise = (uint8_t)(val & ~was), fall = (uint8_t)(was & ~val);
        struct { uint8_t bit; int op1, op2; } K[5] = {
            {0x10, 12, 15},   /* bass drum: both ops of channel 6 */
            {0x08, 16, -1},   /* snare  */
            {0x04, 14, -1},   /* tomtom */
            {0x02, 17, -1},   /* cymbal */
            {0x01, 13, -1},   /* hi-hat */
        };
        for (int i = 0; i < 5; i++) {
            if (rise & K[i].bit) {
                op_key(&o->op[K[i].op1], 1);
                if (K[i].op2 >= 0) op_key(&o->op[K[i].op2], 1);
            } else if (fall & K[i].bit) {
                op_key(&o->op[K[i].op1], 0);
                if (K[i].op2 >= 0) op_key(&o->op[K[i].op2], 0);
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
        case 0x20: p->am = val >> 7; p->vib = (val >> 6) & 1; p->egt = (val >> 5) & 1;
                   p->ksr = (val >> 4) & 1; p->mult = val & 15; break;
        case 0x40: p->ksl = val >> 6; p->tl = val & 63; break;
        case 0x60: p->ar = val >> 4; p->dr = val & 15; break;
        case 0x80: p->sl = val >> 4; p->rr = val & 15; break;
        case 0xE0: p->wave = val & 3; break;
        }
        return;
    }
    if (reg >= 0xA0 && reg <= 0xA8) {
        Ch *c = &o->ch[reg - 0xA0];
        c->fnum = (uint16_t)((c->fnum & 0x300) | val);
    } else if (reg >= 0xB0 && reg <= 0xB8) {
        Ch *c = &o->ch[reg - 0xB0];
        c->fnum = (uint16_t)((c->fnum & 0xFF) | ((val & 3) << 8));
        c->block = (val >> 2) & 7;
        /* rhythm mode: B0 keyon is ignored on channels 6-8 */
        if ((o->bd & 0x20) && reg >= 0xB6) return;
        int on = (val >> 5) & 1;
        if (on && !c->keyon) {
            for (int s = 0; s < 2; s++) {
                Op *p = &o->op[OPMAP[reg - 0xB0][s]];
                p->stage = 1;
                p->phase = 0;
            }
        } else if (!on && c->keyon) {
            for (int s = 0; s < 2; s++) o->op[OPMAP[reg - 0xB0][s]].stage = 4;
        }
        c->keyon = (uint8_t)on;
    } else if (reg >= 0xC0 && reg <= 0xC8) {
        Ch *c = &o->ch[reg - 0xC0];
        c->fb = (val >> 1) & 7;
        c->cnt = val & 1;
    }
}

static void env_step(Op *p, int srate) {
    switch (p->stage) {
    case 1: {
        double r = env_rate(p->ar, srate, 1);
        if (p->ar >= 15) p->env = 1.0;
        else p->env += r;
        if (p->env >= 1.0) { p->env = 1.0; p->stage = 2; }
        break;
    }
    case 2: {
        double sl = 1.0 - p->sl / 15.0;
        p->env -= env_rate(p->dr, srate, 0);
        if (p->env <= sl) { p->env = sl; p->stage = 3; }
        break;
    }
    case 3:
        if (!p->egt) {              /* percussive: keep decaying */
            p->env -= env_rate(p->rr, srate, 0);
            if (p->env <= 0) { p->env = 0; p->stage = 0; }
        }
        break;
    case 4:
        p->env -= env_rate(p->rr, srate, 0);
        if (p->env <= 0) { p->env = 0; p->stage = 0; }
        break;
    }
    if (p->env < 0) p->env = 0;
}

static int16_t op_run(WOpl *o, Op *p, uint32_t inc, int16_t mod) {
    p->phase += inc;
    uint32_t ph = ((p->phase >> 10) + (uint32_t)(mod >> 1)) & 1023;
    int16_t s = sintab[p->wave][ph];
    double att = pow(10.0, -(p->tl * 0.75) / 20.0);   /* total level, 0.75 dB/step */
    env_step(p, o->rate);
    return (int16_t)(s * p->env * att);
}

static const double MUL[16] = {0.5,1,2,3,4,5,6,7,8,9,10,10,12,12,15,15};

static uint32_t ch_inc(const WOpl *o, const Ch *ch, const Op *p) {
    double base = (double)ch->fnum * (1 << ch->block) * 49716.0 / 1048576.0;
    return (uint32_t)(base * MUL[p->mult] / o->rate * 1024.0 * 1024.0);
}

/* percussion operator: envelope + amplitude as usual, but the waveform is
 * replaced by the noise generator (snare/hihat/cymbal are noise-based on
 * the real chip; the tom-tom stays tonal and uses op_run) */
static int16_t op_noise(WOpl *o, Op *p, uint32_t inc) {
    p->phase += inc;
    int16_t s = (o->lfsr & 1) ? 4084 : -4084;
    double att = pow(10.0, -(p->tl * 0.75) / 20.0);
    env_step(p, o->rate);
    return (int16_t)(s * p->env * att * 0.5);
}

void wopl_render(WOpl *o, int16_t *out, int n) {
    if (!o) { memset(out, 0, (size_t)n * 2); return; }
    int rhythm = o->bd & 0x20;
    for (int i = 0; i < n; i++) {
        int acc = 0;
        /* clock the 23-bit noise LFSR once per sample */
        uint32_t fb = ((o->lfsr >> 14) ^ o->lfsr) & 1;
        o->lfsr = (o->lfsr >> 1) | (fb << 22);
        if (!o->lfsr) o->lfsr = 1;
        int melodic = rhythm ? 6 : CHS;
        for (int c = 0; c < melodic; c++) {
            Ch *ch = &o->ch[c];
            Op *m = &o->op[OPMAP[c][0]], *k = &o->op[OPMAP[c][1]];
            if (m->stage == 0 && k->stage == 0) continue;
            uint32_t incm = ch_inc(o, ch, m), inck = ch_inc(o, ch, k);
            int16_t fbmod = ch->fb ? (int16_t)((m->prev + m->out) >> (9 - ch->fb)) : 0;
            int16_t mo = op_run(o, m, incm, fbmod);
            m->prev = m->out;
            m->out = mo;
            if (ch->cnt) {                    /* additive */
                acc += mo + op_run(o, k, inck, 0);
            } else {                          /* FM */
                acc += op_run(o, k, inck, mo);
            }
        }
        if (rhythm) {
            /* bass drum: channel 6 as a normal FM pair, keyed via 0xBD */
            Ch *c6 = &o->ch[6];
            Op *m = &o->op[12], *k = &o->op[15];
            if (m->stage || k->stage) {
                int16_t fbmod = c6->fb ? (int16_t)((m->prev + m->out) >> (9 - c6->fb)) : 0;
                int16_t mo = op_run(o, m, ch_inc(o, c6, m), fbmod);
                m->prev = m->out;
                m->out = mo;
                acc += op_run(o, k, ch_inc(o, c6, k), mo) * 2;
            }
            /* hi-hat (op13) and snare (op16) run off channel 7's frequency,
             * tom-tom (op14, tonal) and cymbal (op17) off channel 8's */
            Op *hh = &o->op[13], *sd = &o->op[16];
            Op *tt = &o->op[14], *cy = &o->op[17];
            if (hh->stage) acc += op_noise(o, hh, ch_inc(o, &o->ch[7], hh));
            if (sd->stage) acc += op_noise(o, sd, ch_inc(o, &o->ch[7], sd)) * 2;
            if (tt->stage) acc += op_run(o, tt, ch_inc(o, &o->ch[8], tt), 0) * 2;
            if (cy->stage) acc += op_noise(o, cy, ch_inc(o, &o->ch[8], cy));
        }
        if (acc > 32767) acc = 32767;
        if (acc < -32768) acc = -32768;
        out[i] = (int16_t)acc;
    }
}
