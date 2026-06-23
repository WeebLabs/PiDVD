/* DVD sub-picture (SPU) decoder: menu graphics, button highlights,
 * subtitles. Assembles SPU packets from the demuxed private stream,
 * RLE-decodes the 4-color bitmap, and renders an RGBA overlay through
 * the program CLUT, with button-highlight recoloring. */
#ifndef PIDVD_DECODE_SPU_H
#define PIDVD_DECODE_SPU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    int x, y, w, h;
    const uint8_t *rgba;   /* w*h*4, valid until next spu call */
} pidvd_overlay_t;

typedef struct pidvd_spu pidvd_spu_t;

pidvd_spu_t *pidvd_spu_new(void);
void pidvd_spu_free(pidvd_spu_t *s);

void pidvd_spu_set_clut(pidvd_spu_t *s, const uint32_t clut[16]);
/* substream 0-31 from the demuxer; only the selected one is decoded */
void pidvd_spu_select_stream(pidvd_spu_t *s, int substream);
/* feed one demuxed private-stream-1 payload (substream id stripped) */
void pidvd_spu_packet(pidvd_spu_t *s, int substream,
                      const uint8_t *data, size_t len);

/* Button highlight: recolor sx..ex/sy..ey using the highlight palette
 * word (color/alpha nibbles). Returns true if the overlay changed. */
bool pidvd_spu_set_highlight(pidvd_spu_t *s, int sx, int sy, int ex,
                             int ey, uint32_t palette);
bool pidvd_spu_clear_highlight(pidvd_spu_t *s);
void pidvd_spu_clear(pidvd_spu_t *s);

/* True once per newly decoded overlay (consumes the flag); *dur_ticks gets the
 * subtitle's explicit display duration in 90kHz ticks, or 0 if it carries no
 * timed stop and the caller should apply its own fallback. */
bool pidvd_spu_fresh(pidvd_spu_t *s, int64_t *dur_ticks);

/* Current overlay, or false if nothing to show. */
bool pidvd_spu_overlay(pidvd_spu_t *s, pidvd_overlay_t *out);

#endif
