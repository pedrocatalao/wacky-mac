#include "wacky.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

bool wdat_open(WDat *dat, const char *path) {
    memset(dat, 0, sizeof *dat);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 2) { fclose(f); return false; }
    dat->data = malloc((size_t)sz);
    dat->size = (size_t)sz;
    if (!dat->data || fread(dat->data, 1, dat->size, f) != dat->size) {
        fclose(f);
        wdat_close(dat);
        return false;
    }
    fclose(f);

    dat->count = rd16(dat->data);
    if (2 + (size_t)dat->count * 22 > dat->size) { wdat_close(dat); return false; }
    dat->entries = calloc((size_t)dat->count, sizeof *dat->entries);
    if (!dat->entries) { wdat_close(dat); return false; }
    const uint8_t *p = dat->data + 2;
    for (int i = 0; i < dat->count; i++, p += 22) {
        memcpy(dat->entries[i].name, p, 14);
        dat->entries[i].name[14] = 0;
        dat->entries[i].size = rd32(p + 14);
        dat->entries[i].offset = rd32(p + 18);
        if ((size_t)dat->entries[i].offset + dat->entries[i].size > dat->size) {
            wdat_close(dat);
            return false;
        }
    }
    return true;
}

void wdat_close(WDat *dat) {
    free(dat->data);
    free(dat->entries);
    memset(dat, 0, sizeof *dat);
}

const uint8_t *wdat_find(const WDat *dat, const char *name, uint32_t *size_out) {
    for (int i = 0; i < dat->count; i++) {
        if (strcasecmp(dat->entries[i].name, name) == 0) {
            if (size_out) *size_out = dat->entries[i].size;
            /* file content starts 2 bytes after the TOC offset */
            return dat->data + dat->entries[i].offset + 2;
        }
    }
    return NULL;
}
