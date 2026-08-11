/* Minimal OPL2 (YM3812) synthesiser interface — see src/opl2.c.
 * KLM music streams are OPL register writes, so the player just feeds
 * wopl_write() and pulls audio with wopl_render(). */
#ifndef WOPL2_H
#define WOPL2_H
#include <stdint.h>
#include <stdlib.h>

typedef struct WOpl WOpl;
WOpl *wopl_create(int rate);
void  wopl_free(WOpl *o);
void  wopl_reset(WOpl *o);
void  wopl_write(WOpl *o, uint8_t reg, uint8_t val);
void  wopl_render(WOpl *o, int16_t *out, int n);
#endif
