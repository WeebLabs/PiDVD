/* Disc-loading animation. See load_anim.h. Stage 1: a static line-art
 * tray-loading DVD player (matching the reference icon), tray open with the
 * chosen disc resting on it. Motion is layered on next. */
#include "ui/load_anim.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>

/* A 2 px stroke (Bresenham with a 2x2 nib) — stays visible through the 2:1
 * menu decimation, matching the UI's 2 px rules. */
static void aline(ui_canvas_t *c, int x0, int y0, int x1, int y1, uint32_t col)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        ui_fill(c, x0, y0, 2, 2, col);
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* Filled disc of radius r in `col`, with a round centre hole punched back to
 * `bg` — the filled-platter-with-hole look of the list icon, but smooth. */
static void adisc(ui_canvas_t *c, int cx, int cy, int r,
                  uint32_t col, uint32_t bg)
{
    int hole = r / 4;
    if (hole < 3)
        hole = 3;
    for (int dy = -r; dy <= r; dy++) {
        int w = (int)(sqrtf((float)(r * r - dy * dy)) + 0.5f);
        ui_fill(c, cx - w, cy + dy, 2 * w, 1, col);
        if (abs(dy) <= hole) {
            int hw = (int)(sqrtf((float)(hole * hole - dy * dy)) + 0.5f);
            ui_fill(c, cx - hw, cy + dy, 2 * hw, 1, bg);
        }
    }
}

/* Line-art tray-loading DVD player, centred in the canvas, tray slid out with
 * the disc (the hot accent) resting on it. */
static void draw_player(ui_canvas_t *c, const ui_theme_t *th)
{
    uint32_t ink = th->bright;
    int cx = c->w / 2;
    int top = c->h / 2 - 70;
    int bw = 260, bh = 80;
    int x0 = cx - bw / 2, y0 = top, x1 = x0 + bw, y1 = y0 + bh;
    int ddx = 44, ddy = 30;            /* 3D depth toward the upper-right */

    /* front face */
    aline(c, x0, y0, x1, y0, ink);
    aline(c, x1, y0, x1, y1, ink);
    aline(c, x0, y1, x1, y1, ink);
    aline(c, x0, y0, x0, y1, ink);
    /* top face */
    aline(c, x0, y0, x0 + ddx, y0 - ddy, ink);
    aline(c, x1, y0, x1 + ddx, y0 - ddy, ink);
    aline(c, x0 + ddx, y0 - ddy, x1 + ddx, y0 - ddy, ink);
    /* right face */
    aline(c, x1, y1, x1 + ddx, y1 - ddy, ink);
    aline(c, x1 + ddx, y0 - ddy, x1 + ddx, y1 - ddy, ink);
    /* two indicator dots on the front */
    ui_fill(c, x1 - 46, y1 - 16, 6, 6, th->dim);
    ui_fill(c, x1 - 30, y1 - 16, 6, 6, th->dim);

    /* open tray: a shallow trapezoid out front, a touch wider at the lip */
    int fy = y1 + 92, fx0 = x0 - 28, fx1 = x1 + 28;
    aline(c, x0, y1, fx0, fy, ink);
    aline(c, x1, y1, fx1, fy, ink);
    aline(c, fx0, fy, fx1, fy, ink);

    /* the disc, resting on the tray */
    adisc(c, cx, y1 + 44, 42, th->hot, th->bg);
}

void pidvd_load_anim_render(ui_canvas_t *c, const ui_theme_t *th,
                            int sx, int sy, unsigned t)
{
    (void)sx; (void)sy; (void)t;   /* stage 1: static graphic for sign-off */
    ui_fill(c, 0, 0, c->w, c->h, th->bg);
    draw_player(c, th);
}
