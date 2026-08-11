/* KLM music player — the game's AdLib music driver ported from WW.EXE
 * (FUN_00050374 init, FUN_00050478 instrument, FUN_000505d0 volume,
 * FUN_00050660 note on, FUN_0005074c note off, FUN_00050790 re-key,
 * FUN_00050280 volume register update), driving the OPL2 core. */
#ifndef WKLM_H
#define WKLM_H
#include <stdbool.h>
#include <stdint.h>

typedef struct WKlm WKlm;

WKlm *wklm_create(int rate);
void  wklm_free(WKlm *k);
/* start a song; data must stay valid while it plays (it is read in place).
 * loop=0 stops at the terminal 0xFF like the original's one-shot jingles */
bool  wklm_start(WKlm *k, const uint8_t *data, uint32_t len, int loop);
void  wklm_stop(WKlm *k);
/* mono S16 at the create() rate; silence when stopped */
void  wklm_render(WKlm *k, int16_t *out, int n);
/* diagnostic tap: observe every OPL register write the driver makes */
void  wklm_set_logger(WKlm *k, void (*fn)(void *ud, uint8_t reg, uint8_t val),
                      void *ud);
#endif
