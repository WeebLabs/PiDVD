/* Framebuffer primitives for the picker UI. All horizontal strokes the
 * callers can produce are >= 2 scanlines (rules are 2 px, text rows are
 * doubled) — interlace law #1, docs/UI.md §1. Pure pixel code: works on
 * any XRGB8888 buffer, host or target. */
#ifndef PIDVD_UI_DRAW_H
#define PIDVD_UI_DRAW_H

#include <stdint.h>

typedef struct {
    uint8_t *px;     /* XRGB8888, little-endian */
    int w, h;
    int stride;      /* bytes */
} ui_canvas_t;

void ui_fill(ui_canvas_t *c, int x, int y, int w, int h, uint32_t color);
/* 2 px horizontal rule */
void ui_hline2(ui_canvas_t *c, int x, int y, int w, uint32_t color);
/* 2 px vertical rule */
void ui_vline2(ui_canvas_t *c, int x, int y, int h, uint32_t color);

/* Draw UTF-8 text; each font pixel becomes sx*sy device pixels.
 * sy must be even (interlace law). Returns the x just past the text.
 * clip_x1 <= 0 means no clipping; text never wraps. */
int ui_text(ui_canvas_t *c, int x, int y, int sx, int sy,
            uint32_t color, const char *s);
int ui_text_clip(ui_canvas_t *c, int x, int y, int sx, int sy,
                 uint32_t color, const char *s, int clip_x1);
/* Width in pixels of the rendered string (8*sx per glyph). */
int ui_text_w(const char *s, int sx);

/* Linear blend a->b, t in 0..256 */
uint32_t ui_lerp(uint32_t a, uint32_t b, int t);

#endif
