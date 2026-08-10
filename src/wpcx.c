#include "wacky.h"

#include <stdlib.h>
#include <string.h>

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* Standard ZSoft PCX, 8bpp single plane: 128-byte header, linear RLE body
 * (runs may cross row boundaries), 0x0C marker + 768-byte palette at EOF. */
bool wpcx_decode(const uint8_t *buf, size_t len, WImage *img) {
    memset(img, 0, sizeof *img);
    if (len < 128 + 769 || buf[0] != 0x0A) return false;

    int w = rd16(buf + 8) - rd16(buf + 4) + 1;
    int h = rd16(buf + 10) - rd16(buf + 6) + 1;
    int bpl = rd16(buf + 66);
    if (w <= 0 || h <= 0 || bpl < w) return false;

    memcpy(img->pal, buf + len - 768, 768);

    img->w = w;
    img->h = h;
    img->pixels = malloc((size_t)bpl * h);
    if (!img->pixels) return false;

    size_t need = (size_t)bpl * h, out = 0, i = 128;
    while (out < need && i < len) {
        uint8_t b = buf[i++];
        int run = 1;
        if ((b & 0xC0) == 0xC0) {
            run = b & 0x3F;
            if (i >= len) break;
            b = buf[i++];
        }
        while (run-- && out < need) img->pixels[out++] = b;
    }
    if (out < need) { wimage_free(img); return false; }
    /* callers index by w; repack if bpl > w */
    if (bpl > w)
        for (int y = 1; y < h; y++)
            memmove(img->pixels + (size_t)y * w, img->pixels + (size_t)y * bpl, w);
    return true;
}

void wimage_free(WImage *img) {
    free(img->pixels);
    memset(img, 0, sizeof *img);
}
