#include "ui/draw.h"

#include <stddef.h>

#include "ui/font8.h"

void ui_fill(ui_canvas_t *c, int x, int y, int w, int h, uint32_t color)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > c->w) w = c->w - x;
    if (y + h > c->h) h = c->h - y;
    for (int j = 0; j < h; j++) {
        uint32_t *row = (uint32_t *)(c->px + (size_t)(y + j) * c->stride) + x;
        for (int i = 0; i < w; i++)
            row[i] = color;
    }
}

void ui_hline2(ui_canvas_t *c, int x, int y, int w, uint32_t color)
{
    ui_fill(c, x, y, w, 2, color);
}

void ui_vline2(ui_canvas_t *c, int x, int y, int h, uint32_t color)
{
    ui_fill(c, x, y, 2, h, color);
}

int ui_text_clip(ui_canvas_t *c, int x, int y, int sx, int sy,
                 uint32_t color, const char *s, int clip_x1)
{
    while (*s) {
        uint32_t cp = pidvd_utf8_next(&s);
        const unsigned char *g = pidvd_font8_glyph(cp);
        if (!g)
            g = pidvd_font8[0]; /* unmapped -> space */
        if (clip_x1 > 0 && x + 8 * sx > clip_x1)
            break;
        for (int row = 0; row < 8; row++) {
            unsigned bits = g[row];
            if (!bits)
                continue;
            for (int col = 0; col < 8; col++)
                if (bits & (1u << col))
                    ui_fill(c, x + col * sx, y + row * sy, sx, sy, color);
        }
        x += 8 * sx;
    }
    return x;
}

int ui_text(ui_canvas_t *c, int x, int y, int sx, int sy,
            uint32_t color, const char *s)
{
    return ui_text_clip(c, x, y, sx, sy, color, s, 0);
}

int ui_text_w(const char *s, int sx)
{
    int n = 0;
    while (*s) {
        pidvd_utf8_next(&s);
        n++;
    }
    return n * 8 * sx;
}

uint32_t ui_lerp(uint32_t a, uint32_t b, int t)
{
    uint32_t out = 0;
    for (int sh = 0; sh < 24; sh += 8) {
        int av = (int)((a >> sh) & 0xff), bv = (int)((b >> sh) & 0xff);
        out |= (uint32_t)(av + ((bv - av) * t >> 8)) << sh;
    }
    return out;
}
