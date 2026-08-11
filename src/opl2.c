/* OPL2 synth front-end — a thin mono wrapper over the Nuked OPL3 core
 * (src/nuked_opl3.c, LGPL 2.1, (C) Nuke.YKT), which is a bit-accurate
 * emulation of the Yamaha FM chips derived from die shots. The KLM driver
 * feeds it the same register writes the game fed the real card, so the
 * music renders exactly as it did on hardware.
 *
 * The YMF262 in its OPL2-compatible mode (NEW bit clear) is how every
 * later sound card ran this game's music, and Nuked emulates that mode
 * including rhythm, vibrato/tremolo depth and envelope behaviour.
 */
#include "opl2.h"

#include "nuked_opl3.h"
#include <stdlib.h>
#include <string.h>

struct WOpl {
    opl3_chip chip;
    int rate;
};

WOpl *wopl_create(int rate) {
    WOpl *o = calloc(1, sizeof *o);
    if (!o) return NULL;
    o->rate = rate;
    OPL3_Reset(&o->chip, (uint32_t)rate);
    return o;
}

void wopl_free(WOpl *o) { free(o); }

void wopl_reset(WOpl *o) {
    if (!o) return;
    OPL3_Reset(&o->chip, (uint32_t)o->rate);
}

void wopl_write(WOpl *o, uint8_t reg, uint8_t val) {
    if (!o) return;
    OPL3_WriteRegBuffered(&o->chip, reg, val);
}

void wopl_render(WOpl *o, int16_t *out, int n) {
    if (!o) { memset(out, 0, (size_t)n * 2); return; }
    for (int i = 0; i < n; i++) {
        int16_t pair[2];
        OPL3_GenerateResampled(&o->chip, pair);
        int v = ((int)pair[0] + pair[1]) >> 1;
        out[i] = (int16_t)v;
    }
}
