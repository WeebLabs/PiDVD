/* Picker rendering: every screen, all three layouts, themed. Pure pixel
 * code over ui_canvas_t — see render.h. Geometry follows docs/UI.md:
 * 48 px side margins, 38/32 px vertical margins, all horizontal strokes
 * >= 2 scanlines. */
#include "ui/render.h"

#include <stdio.h>

#include "ui/font8.h"
#include "ui/saver.h"

/* docs/UI.md §2 — AMBER & ICE, PHOSPHOR, VFD, MIDNIGHT, TERMINAL.
 * Columns: bg panel dim text bright hot bar bartxt */
const ui_theme_t pidvd_themes[PIDVD_N_THEMES] = {
    { 0x0D0A06, 0x161310, 0x4E6A86, 0xD98E00, 0xF4EFE2, 0x8FC6FF,
      0xFFA000, 0x1A0E00 },
    /* PHOSPHOR — pure-mono amber phosphor, nudged slightly redder
     * (green ~-12% vs the amber themes; R/B unchanged). */
    { 0x0E0600, 0x1C0E00, 0x6E3C00, 0xD97C00, 0xFF9A00, 0xFFC29C,
      0xFF8C00, 0x140900 },
    { 0x03100D, 0x07201B, 0x1F5A50, 0x63D6BE, 0xD9FFF4, 0xFFB000,
      0x49E0C2, 0x03201A },
    { 0x070B14, 0x0D1426, 0x32436B, 0x8FB0E8, 0xEEF2FA, 0xFFB000,
      0x5B86DC, 0x060D1E },
    /* TERMINAL — the redder PHOSPHOR phosphor on a pure-black void, for the
     * wireframe vintage-terminal look (only the bg differs from PHOSPHOR). */
    { 0x000000, 0x1C0E00, 0x6E3C00, 0xD97C00, 0xFF9A00, 0xFFC29C,
      0xFF8C00, 0x140900 },
};

/* UTF-8 literals for the glyph set */
#define G_RIGHT  "\xe2\x96\xb8"  /* ▸ */
#define G_LEFT   "\xe2\x97\x82"  /* ◂ */
#define G_UP     "\xe2\x96\xb4"  /* ▴ */
#define G_DOWN   "\xe2\x96\xbe"  /* ▾ */
#define G_DISC   "\xe2\x97\x89"  /* ◉ */
#define G_DISC2  "\xe2\x97\x8d"  /* ◍ */
#define G_DISC3  "\xe2\x97\x8c"  /* ◌ */
#define G_RESUME "\xe2\x9f\xb3"  /* ⟳ */
#define G_DOT    "\xc2\xb7"      /* · */
#define G_ENTER  "\xe2\x86\xb5"  /* ↵ */
#define G_PGUP   "\xc2\xab"      /* « */
#define G_PGDN   "\xc2\xbb"      /* » */
#define G_ELL    "\xe2\x80\xa6"  /* … */
#define G_STOP   "\xe2\x96\xa0"  /* ■ */
#define G_GEAR   "\xe2\x9a\x99"  /* ⚙ settings */
#define G_BACK   "\xe2\x86\x90"  /* ← back */

/* sizes: SMALL 8x16, LIST 16x16, TITLE 32x32 (docs/UI.md §3) */
#define S_SM_X 1
#define S_SM_Y 2
#define S_LS_X 2
#define S_LS_Y 2
#define S_TI_X 4
#define S_TI_Y 4

typedef struct {
    int x0, x1, y0, y1;  /* safe area */
} geo_t;

static geo_t geo(const ui_canvas_t *c)
{
    geo_t g;
    g.x0 = 48;
    g.x1 = c->w - 48;
    g.y0 = (c->h >= 576) ? 38 : 32;
    g.y1 = c->h - g.y0;
    return g;
}

static const char *spinner(int tick)
{
    static const char *const f[4] = { G_DISC3, G_DISC2, G_DISC, G_DISC2 };
    return f[(tick / 12) & 3];
}

static void fmt_dur_short(double s, char *out, int cap)
{
    int m = (int)(s / 60.0 + 0.5);
    snprintf(out, (size_t)cap, "%d:%02d", m / 60, m % 60);
}

static void fmt_dur_long(double s, char *out, int cap)
{
    int t = (int)s;
    snprintf(out, (size_t)cap, "%d:%02d:%02d", t / 3600, (t / 60) % 60,
             t % 60);
}

static void fmt_size(uint64_t b, char *out, int cap)
{
    double gb = (double)b / (1024.0 * 1024.0 * 1024.0);
    if (gb >= 0.95)
        snprintf(out, (size_t)cap, "%.1f GB", gb);
    else
        snprintf(out, (size_t)cap, "%d MB",
                 (int)((double)b / (1024.0 * 1024.0)));
}

static void fmt_region(uint8_t mask, char *out, int cap)
{
    if (mask == 0xff) {
        snprintf(out, (size_t)cap, "ALL REGIONS");
        return;
    }
    int n = 0;
    int len = snprintf(out, (size_t)cap, "REGION");
    for (int r = 0; r < 8 && len < cap - 3; r++)
        if (mask & (1u << r)) {
            len += snprintf(out + len, (size_t)(cap - len), " %d", r + 1);
            n++;
        }
    if (!n)
        snprintf(out, (size_t)cap, "REGION ?");
}

static void fmt_badge(const ui_item_t *it, char *out, int cap)
{
    snprintf(out, (size_t)cap, "%s %s %di %s %s%s",
             it->standard ? "PAL" : "NTSC", G_DOT, it->height, G_DOT,
             it->wide ? "16:9" : "4:3", it->letterboxed ? " LB" : "");
}

/* mixed-tone " · "-separated strip: fields bright, dots dim */
static int strip(ui_canvas_t *c, int x, int y, int sx, int sy,
                 uint32_t field_col, uint32_t dot_col,
                 const char *const *fields, int n, int clip_x1)
{
    for (int i = 0; i < n; i++) {
        if (!fields[i] || !fields[i][0])
            continue;
        if (i)
            x = ui_text_clip(c, x, y, sx, sy, dot_col, " " G_DOT " ",
                             clip_x1);
        x = ui_text_clip(c, x, y, sx, sy, field_col, fields[i], clip_x1);
    }
    return x;
}

/* ---- logo ------------------------------------------------------------ */

static const char *const logo_rows[6] = {
    "██████▖ ▝█▘ ██████▖ ██▖  ▗██ ██████▖",
    "██  ▝██ ▗▄▖ ██  ▝██ ▝██  ██▘ ██  ▝██",
    "██▄▄██▘ ▐█▌ ██   ██  ▐█▙▟█▌  ██   ██",
    "██▀▀▀   ▐█▌ ██   ██  ▝████▘  ██   ██",
    "██      ▐█▌ ██  ▗██   ▝██▘   ██  ▗██",
    "██      ▝█▘ ██████▘    ▝▘    ██████▘",
};
#define LOGO_COLS 36

static int quad_mask(uint32_t cp)
{
    switch (cp) {
    case 0x2588: return 0xF;            /* █ */
    case 0x2598: return 0x1;            /* ▘ UL */
    case 0x259D: return 0x2;            /* ▝ UR */
    case 0x2596: return 0x4;            /* ▖ LL */
    case 0x2597: return 0x8;            /* ▗ LR */
    case 0x2580: return 0x3;            /* ▀ */
    case 0x2584: return 0xC;            /* ▄ */
    case 0x258C: return 0x5;            /* ▌ */
    case 0x2590: return 0xA;            /* ▐ */
    case 0x2599: return 0xD;            /* ▙ */
    case 0x259F: return 0xE;            /* ▟ */
    default:     return 0;
    }
}

/* cell = s x s px (s even); the i's dot cells render hot */
static void draw_logo(ui_canvas_t *c, int cx, int y, int s,
                      uint32_t main_col, uint32_t hot_col)
{
    int x0 = cx - LOGO_COLS * s / 2;
    for (int row = 0; row < 6; row++) {
        const char *p = logo_rows[row];
        for (int col = 0; *p; col++) {
            uint32_t cp = pidvd_utf8_next(&p);
            int m = quad_mask(cp);
            if (!m)
                continue;
            uint32_t col_c = (row == 0 && col >= 8 && col <= 10)
                                 ? hot_col : main_col;
            int x = x0 + col * s, yy = y + row * s, h = s / 2;
            if (m & 1) ui_fill(c, x, yy, h, h, col_c);
            if (m & 2) ui_fill(c, x + h, yy, h, h, col_c);
            if (m & 4) ui_fill(c, x, yy + h, h, h, col_c);
            if (m & 8) ui_fill(c, x + h, yy + h, h, h, col_c);
        }
    }
}

/* ---- footer hints ----------------------------------------------------- */

typedef struct { const char *glyph, *label; } hint_t;

static void footer(ui_canvas_t *c, const geo_t *g, const ui_theme_t *th,
                   const hint_t *hints, int n, bool centered)
{
    int total = 0;
    for (int i = 0; i < n; i++)
        total += ui_text_w(hints[i].glyph, S_SM_X) + 8
               + ui_text_w(hints[i].label, S_SM_X) + 24;
    int x = centered ? (g->x0 + g->x1 - total) / 2 : g->x0 + 8;
    int y = g->y1 - 18;
    for (int i = 0; i < n; i++) {
        x = ui_text(c, x, y, S_SM_X, S_SM_Y, th->bright, hints[i].glyph);
        x += 8;
        x = ui_text(c, x, y, S_SM_X, S_SM_Y, th->dim, hints[i].label);
        x += 24;
    }
}

/* ---- ATTRACT ----------------------------------------------------------- */

static void render_attract(ui_canvas_t *c, const ui_view_t *v,
                           const ui_theme_t *th)
{
    geo_t g = geo(c);
    int cx = (g.x0 + g.x1) / 2;
    int s = 8; /* logo cell px -> 288x48 */
    int ly = g.y0 + (g.y1 - g.y0) / 2 - 110;

    draw_logo(c, cx, ly, s, th->bright, th->hot);

    const char *tag = "F I E L D   A C C U R A T E   " G_DOT "   1 5 k H z";
    ui_text(c, cx - ui_text_w(tag, S_SM_X) / 2, ly + 6 * s + 14,
            S_SM_X, S_SM_Y, th->dim, tag);

    /* 0.4 Hz-ish pulse between dim and text, never to black */
    int ph = v->tick % 128;
    int tri = ph < 64 ? ph * 4 : (128 - ph) * 4;
    uint32_t pulse = ui_lerp(th->dim, th->text, tri);

    const char *l1 = v->notice ? v->notice : "I N S E R T   D I S C";
    ui_text(c, cx - ui_text_w(l1, S_LS_X) / 2, ly + 6 * s + 90,
            S_LS_X, S_LS_Y, pulse, l1);
    if (!v->notice) {
        const char *l2 = G_RIGHT " USB DRIVE  " G_DOT "  DVD VIDEO ISO";
        ui_text(c, cx - ui_text_w(l2, S_SM_X) / 2, ly + 6 * s + 124,
                S_SM_X, S_SM_Y, th->dim, l2);
    }
}

/* ---- shared browse chrome --------------------------------------------- */

static int header(ui_canvas_t *c, const ui_view_t *v, const ui_theme_t *th,
                  const geo_t *g)
{
    int y = g->y0;
    int x = ui_text(c, g->x0 + 8, y + 2, S_LS_X, S_LS_Y, th->hot, G_DISC);
    ui_text(c, x + 8, y + 2, S_LS_X, S_LS_Y, th->bright, "PiDVD");

    char count[32];
    snprintf(count, sizeof(count), "%d DISC%s", v->n_discs,
             v->n_discs == 1 ? "" : "S");
    ui_text(c, g->x1 - 8 - ui_text_w(count, S_SM_X), y + 8, S_SM_X, S_SM_Y,
            th->dim, count);

    char loc[160];
    snprintf(loc, sizeof(loc), "%s %s /%s", v->source ? v->source : "USB",
             G_DOT, v->path ? v->path : "");
    int lw = ui_text_w(loc, S_SM_X);
    ui_text(c, (g->x0 + g->x1 - lw) / 2, y + 8, S_SM_X, S_SM_Y, th->text,
            loc);

    ui_hline2(c, g->x0, y + 28, g->x1 - g->x0, th->dim);
    return y + 34;
}

static int shelf(ui_canvas_t *c, const ui_view_t *v, const ui_theme_t *th,
                 const geo_t *g, int y)
{
    if (!v->now_playing)
        return y;
    int x = ui_text(c, g->x0 + 8, y + 2, S_SM_X, S_SM_Y, th->dim,
                    G_RIGHT " NOW PLAYING  ");
    ui_text_clip(c, x, y, S_LS_X, S_LS_Y, th->bright, v->now_playing,
                 g->x1 - 140);
    const char *r = G_ENTER " RESUME";
    ui_text(c, g->x1 - 8 - ui_text_w(r, S_SM_X), y + 2, S_SM_X, S_SM_Y,
            th->dim, r);
    ui_hline2(c, g->x0, y + 22, g->x1 - g->x0, th->dim);
    return y + 28;
}

static const char *item_icon(const ui_item_t *it)
{
    if (it->is_parent) return G_LEFT;
    if (it->is_dir)    return G_RIGHT;
    return G_DISC;
}

/* ---- CONSOLE ----------------------------------------------------------- */

#define CON_ROW_H 22

static void info_card(ui_canvas_t *c, const ui_view_t *v,
                      const ui_theme_t *th, const geo_t *g,
                      int x, int yb, int ye, bool fill)
{
    if (fill)
        ui_fill(c, x, yb, g->x1 - x, ye - yb, th->panel);
    if (v->sel < 0 || v->sel >= v->n_items)
        return;
    const ui_item_t *it = v->items[v->sel];
    int lx = x + 12, y = yb + 14;
    int clip = g->x1 - 8;
    char b1[64], b2[64], b3[64];

    if (it->is_parent)
        return;

    ui_text_clip(c, lx, y, S_LS_X, S_LS_Y, th->bright, it->name, clip);
    y += 24;

    if (it->is_dir) {
        fmt_size(it->size, b2, sizeof(b2));
        snprintf(b1, sizeof(b1), "%d ITEMS %s %s", it->n_items, G_DOT,
                 it->size ? b2 : "");
        ui_text_clip(c, lx, y + 8, S_SM_X, S_SM_Y, th->text, b1, clip);
        return;
    }

    ui_text_clip(c, lx, y, S_SM_X, S_SM_Y, th->dim, it->volid, clip);
    y += 30;

    if (!it->scanned) {
        char ln[64];
        snprintf(ln, sizeof(ln), "%s READING DISC%s", spinner(v->tick),
                 G_ELL);
        ui_text_clip(c, lx, y, S_SM_X, S_SM_Y, th->text, ln, clip);
        return;
    }
    if (it->scan_failed) {
        ui_text_clip(c, lx, y, S_SM_X, S_SM_Y, th->dim, "NOT A DVD IMAGE",
                     clip);
        return;
    }

    fmt_badge(it, b1, sizeof(b1));
    ui_text_clip(c, lx, y, S_SM_X, S_SM_Y, th->bright, b1, clip);
    y += 22;
    fmt_region(it->region_mask, b2, sizeof(b2));
    fmt_size(it->size, b3, sizeof(b3));
    const char *f2[2] = { b2, b3 };
    strip(c, lx, y, S_SM_X, S_SM_Y, th->bright, th->dim, f2, 2, clip);
    y += 30;

    snprintf(b1, sizeof(b1), "%d TITLE%s", it->titles,
             it->titles == 1 ? "" : "S");
    snprintf(b2, sizeof(b2), "%d CHAPTERS", it->chapters);
    const char *f3[2] = { b1, b2 };
    strip(c, lx, y, S_SM_X, S_SM_Y, th->bright, th->dim, f3, 2, clip);
    y += 22;
    int x2 = ui_text(c, lx, y, S_SM_X, S_SM_Y, th->dim, "LONGEST  ");
    fmt_dur_long(it->longest, b1, sizeof(b1));
    ui_text(c, x2, y, S_SM_X, S_SM_Y, th->bright, b1);
    y += 30;

    for (int a = 0; a < it->n_audio && a < 3; a++) {
        x2 = ui_text(c, lx, y, S_SM_X, S_SM_Y, th->dim,
                     a == 0 ? "AUDIO  " : "       ");
        ui_text_clip(c, x2, y, S_SM_X, S_SM_Y, th->bright, it->audio[a],
                     clip);
        y += 22;
    }
    if (it->subs[0]) {
        x2 = ui_text(c, lx, y, S_SM_X, S_SM_Y, th->dim, "SUBS   ");
        ui_text_clip(c, x2, y, S_SM_X, S_SM_Y, th->bright, it->subs, clip);
    }
}

static void render_console(ui_canvas_t *c, const ui_view_t *v,
                           const ui_theme_t *th)
{
    geo_t g = geo(c);
    int yb = header(c, v, th, &g);
    yb = shelf(c, v, th, &g, yb);
    int ye = g.y1 - 26;
    int list_w = 384;
    int lx1 = g.x0 + list_w;

    ui_vline2(c, lx1 + 4, yb, ye - yb, th->dim);
    info_card(c, v, th, &g, lx1 + 10, yb, ye, true);

    int rows = (ye - yb) / CON_ROW_H;
    for (int r = 0; r < rows; r++) {
        int idx = v->scroll + r;
        if (idx >= v->n_items)
            break;
        const ui_item_t *it = v->items[idx];
        int y = yb + r * CON_ROW_H;
        bool sel = (idx == v->sel);
        uint32_t fg = sel ? th->bartxt : th->text;
        uint32_t fg2 = sel ? th->bartxt : th->dim;
        if (sel)
            ui_fill(c, g.x0, y, list_w, CON_ROW_H, th->bar);

        ui_text(c, g.x0 + 8, y + 3, S_LS_X, S_LS_Y, fg, item_icon(it));
        char right[24] = "";
        if (it->is_dir && !it->is_parent)
            snprintf(right, sizeof(right), "%d", it->n_items);
        else if (!it->is_dir && it->scanned && !it->scan_failed)
            fmt_dur_short(it->longest, right, sizeof(right));
        else if (!it->is_dir && !it->scanned)
            snprintf(right, sizeof(right), "%s", spinner(v->tick + idx));
        int rw = ui_text_w(right, S_SM_X);
        ui_text(c, g.x0 + list_w - 10 - rw, y + 4, S_SM_X, S_SM_Y, fg2,
                right);
        ui_text_clip(c, g.x0 + 36, y + 3, S_LS_X, S_LS_Y, fg, it->name,
                     g.x0 + list_w - 16 - rw);
    }

    /* scrollbar between list and divider */
    if (v->n_items > rows && rows > 0) {
        int track_y = yb, track_h = ye - yb;
        ui_fill(c, lx1 - 2, track_y, 4, track_h, th->panel);
        int th_h = track_h * rows / v->n_items;
        if (th_h < 24) th_h = 24;
        int th_y = track_y
                 + (track_h - th_h) * v->scroll
                   / (v->n_items - rows > 0 ? v->n_items - rows : 1);
        ui_fill(c, lx1 - 2, th_y, 4, th_h, th->dim);
    }

    ui_hline2(c, g.x0, ye + 4, g.x1 - g.x0, th->dim);
    bool on_dir = v->sel >= 0 && v->sel < v->n_items
               && v->items[v->sel]->is_dir;
    hint_t h[5] = {
        { G_UP G_DOWN, "SELECT" },
        { G_ENTER, on_dir ? "OPEN" : "PLAY" },
        { v->at_root ? G_GEAR : G_BACK, v->at_root ? "SETTINGS" : "BACK" },
        { G_LEFT " " G_RIGHT, "PAGE" },
        { G_STOP, "EJECT" },
    };
    footer(c, &g, th, h, 5, false);
}

/* ---- WIREFRAME --------------------------------------------------------
 * Console structure drawn as line-art: an outer frame + ruled dividers,
 * NO background fills anywhere; the selected row is marked by an outline
 * box instead of an inverse bar. A real vintage-terminal look. Colour is
 * the active theme — lines in DIM, selection + selected text in BRIGHT. */

static void wf_box(ui_canvas_t *c, int x, int y, int w, int h, uint32_t col)
{
    ui_hline2(c, x, y, w, col);
    ui_hline2(c, x, y + h - 2, w, col);
    ui_vline2(c, x, y, h, col);
    ui_vline2(c, x + w - 2, y, h, col);
}

static void render_wireframe(ui_canvas_t *c, const ui_view_t *v,
                             const ui_theme_t *th)
{
    geo_t g = geo(c);
    /* Background comes from the theme (the TERMINAL theme is a pure-black
     * void for the classic line-art-on-black terminal look). */
    wf_box(c, g.x0, g.y0, g.x1 - g.x0, g.y1 - g.y0, th->dim);

    int yb = header(c, v, th, &g);   /* logo/path/count + full-width rule */
    yb = shelf(c, v, th, &g, yb);    /* NOW PLAYING strip + rule, if any  */
    int ye = g.y1 - 28;
    int list_w = 384;
    int vx = g.x0 + list_w;

    ui_vline2(c, vx, yb, ye - yb, th->dim);           /* list | info       */
    ui_hline2(c, g.x0, ye, g.x1 - g.x0, th->dim);     /* footer divider    */
    info_card(c, v, th, &g, vx + 12, yb, ye, false);  /* no panel fill     */

    int rows = (ye - yb) / CON_ROW_H;
    for (int r = 0; r < rows; r++) {
        int idx = v->scroll + r;
        if (idx >= v->n_items)
            break;
        const ui_item_t *it = v->items[idx];
        int y = yb + r * CON_ROW_H + 1;
        bool sel = (idx == v->sel);
        uint32_t fg = sel ? th->bright : th->text;
        uint32_t fg2 = sel ? th->bright : th->dim;
        if (sel)   /* outline the row instead of filling it */
            wf_box(c, g.x0 + 6, y - 1, list_w - 16, CON_ROW_H, th->bright);

        ui_text(c, g.x0 + 14, y + 2, S_LS_X, S_LS_Y, fg, item_icon(it));
        char right[24] = "";
        if (it->is_dir && !it->is_parent)
            snprintf(right, sizeof(right), "%d", it->n_items);
        else if (!it->is_dir && it->scanned && !it->scan_failed)
            fmt_dur_short(it->longest, right, sizeof(right));
        else if (!it->is_dir && !it->scanned)
            snprintf(right, sizeof(right), "%s", spinner(v->tick + idx));
        int rw = ui_text_w(right, S_SM_X);
        ui_text(c, g.x0 + list_w - 16 - rw, y + 3, S_SM_X, S_SM_Y, fg2, right);
        ui_text_clip(c, g.x0 + 42, y + 2, S_LS_X, S_LS_Y, fg, it->name,
                     g.x0 + list_w - 22 - rw);
    }

    bool on_dir = v->sel >= 0 && v->sel < v->n_items
               && v->items[v->sel]->is_dir;
    hint_t h[5] = {
        { G_UP G_DOWN, "SELECT" },
        { G_ENTER, on_dir ? "OPEN" : "PLAY" },
        { v->at_root ? G_GEAR : G_BACK, v->at_root ? "SETTINGS" : "BACK" },
        { G_LEFT " " G_RIGHT, "PAGE" },
        { G_STOP, "EJECT" },
    };
    footer(c, &g, th, h, 5, false);
}

/* ---- MARQUEE ----------------------------------------------------------- */

static void marquee_name(const ui_item_t *it, char *out, int cap)
{
    if (it->is_parent)
        snprintf(out, (size_t)cap, G_LEFT " BACK");
    else if (it->is_dir)
        snprintf(out, (size_t)cap, G_RIGHT " %s %s %d", it->name, G_DOT,
                 it->n_items);
    else
        snprintf(out, (size_t)cap, "%s", it->name);
}

static void render_marquee(ui_canvas_t *c, const ui_view_t *v,
                           const ui_theme_t *th)
{
    geo_t g = geo(c);
    int cx = (g.x0 + g.x1) / 2;

    /* one-line header, centered */
    {
        char loc[96], cnt[32];
        snprintf(loc, sizeof(loc), "PiDVD %s %s %s /%s", G_DOT,
                 v->source ? v->source : "USB", G_DOT,
                 v->path ? v->path : "");
        snprintf(cnt, sizeof(cnt), " %s %d DISCS", G_DOT, v->n_discs);
        int w = 20 + ui_text_w(loc, S_SM_X) + ui_text_w(cnt, S_SM_X);
        int x = cx - w / 2;
        x = ui_text(c, x, g.y0 + 4, S_SM_X, S_SM_Y, th->hot, G_DISC);
        x = ui_text(c, x + 8, g.y0 + 4, S_SM_X, S_SM_Y, th->text, loc);
        ui_text(c, x, g.y0 + 4, S_SM_X, S_SM_Y, th->dim, cnt);
    }

    if (v->n_items <= 0)
        return;

    int yc = (g.y0 + g.y1) / 2 - 36;
    char nm[128];

    /* neighbors fade with distance; wrap when the list is long enough */
    static const int off_y[5] = { -78, -48, 0, 40, 70 };
    for (int d = -2; d <= 2; d++) {
        int idx = v->sel + d;
        if (v->n_items >= 5)
            idx = (idx % v->n_items + v->n_items) % v->n_items;
        else if (idx < 0 || idx >= v->n_items)
            continue;
        const ui_item_t *it = v->items[idx];
        marquee_name(it, nm, sizeof(nm));
        if (d == 0) {
            int sx = S_TI_X, sy = S_TI_Y;
            if (ui_text_w(nm, sx) > g.x1 - g.x0 - 120) {
                sx = S_LS_X; sy = S_LS_Y;
            }
            int w = ui_text_w(nm, sx);
            int y = yc - sy * 4;
            ui_text(c, cx - w / 2 - 36, yc - 8, S_LS_X, S_LS_Y, th->bar,
                    G_RIGHT);
            ui_text(c, cx + w / 2 + 20, yc - 8, S_LS_X, S_LS_Y, th->bar,
                    G_LEFT);
            ui_text(c, cx - w / 2, y, sx, sy, th->bright, nm);
        } else {
            uint32_t col = (d == -1 || d == 1) ? th->text : th->dim;
            int w = ui_text_w(nm, S_LS_X);
            ui_text(c, cx - w / 2, yc + off_y[d + 2], S_LS_X, S_LS_Y, col,
                    nm);
        }
    }

    /* essentials line */
    const ui_item_t *it = v->items[v->sel];
    if (!it->is_dir && it->scanned && !it->scan_failed) {
        char badge[64], dur[16], reg[32];
        fmt_badge(it, badge, sizeof(badge));
        fmt_dur_short(it->longest, dur, sizeof(dur));
        fmt_region(it->region_mask, reg, sizeof(reg));
        const char *f[5] = { badge, dur, it->n_audio ? it->audio[0] : 0,
                             reg, 0 };
        int w = 0;
        for (int i = 0; i < 4; i++)
            if (f[i]) w += ui_text_w(f[i], S_SM_X) + (i ? 24 : 0);
        strip(c, cx - w / 2, yc + 116, S_SM_X, S_SM_Y, th->bright, th->dim,
              f, 4, g.x1);
    } else if (!it->is_dir && !it->scanned) {
        char ln[64];
        snprintf(ln, sizeof(ln), "%s READING DISC%s", spinner(v->tick),
                 G_ELL);
        ui_text(c, cx - ui_text_w(ln, S_SM_X) / 2, yc + 116, S_SM_X, S_SM_Y,
                th->text, ln);
    }

    if (v->now_playing) {
        char ln[128];
        snprintf(ln, sizeof(ln), "II %s %s %s RESUME", v->now_playing,
                 G_DOT, G_ENTER);
        ui_text(c, cx - ui_text_w(ln, S_SM_X) / 2, g.y1 - 46, S_SM_X,
                S_SM_Y, th->dim, ln);
    }

    hint_t h[3] = {
        { G_UP G_DOWN, "SELECT" },
        { G_ENTER, "PLAY" },
        { v->at_root ? G_GEAR : G_BACK, v->at_root ? "SETTINGS" : "BACK" },
    };
    footer(c, &g, th, h, 3, true);
}

/* ---- LEDGER ------------------------------------------------------------ */

#define LED_ROW_H 20

static void render_ledger(ui_canvas_t *c, const ui_view_t *v,
                          const ui_theme_t *th)
{
    geo_t g = geo(c);
    int yb = header(c, v, th, &g);
    yb = shelf(c, v, th, &g, yb);

    int x_name = g.x0 + 28, x_std = g.x0 + 344, x_asp = g.x0 + 408;
    int x_len = g.x0 + 532, x_sz = g.x1 - 8; /* right-aligned columns */

    ui_text(c, x_name, yb, S_SM_X, S_SM_Y, th->dim, "NAME");
    ui_text(c, x_std, yb, S_SM_X, S_SM_Y, th->dim, "STD");
    ui_text(c, x_asp, yb, S_SM_X, S_SM_Y, th->dim, "ASPECT");
    ui_text(c, x_len - 48, yb, S_SM_X, S_SM_Y, th->dim, "LENGTH");
    ui_text(c, x_sz - ui_text_w("SIZE", S_SM_X), yb, S_SM_X, S_SM_Y,
            th->dim, "SIZE");
    yb += LED_ROW_H + 4;

    int ye = g.y1 - 52;
    int rows = (ye - yb) / LED_ROW_H;
    for (int r = 0; r < rows; r++) {
        int idx = v->scroll + r;
        if (idx >= v->n_items)
            break;
        const ui_item_t *it = v->items[idx];
        int y = yb + r * LED_ROW_H;
        bool sel = (idx == v->sel);
        uint32_t fg = sel ? th->bartxt : th->text;
        uint32_t fg2 = sel ? th->bartxt : th->dim;
        if (sel)
            ui_fill(c, g.x0, y, g.x1 - g.x0, LED_ROW_H, th->bar);

        ui_text(c, g.x0 + 6, y + 2, S_SM_X, S_SM_Y, fg, item_icon(it));
        ui_text_clip(c, x_name, y + 2, S_SM_X, S_SM_Y, fg, it->name,
                     x_std - 12);
        char col[24];
        if (it->is_dir && !it->is_parent) {
            ui_text(c, x_std, y + 2, S_SM_X, S_SM_Y, fg2, G_DOT);
            ui_text(c, x_asp, y + 2, S_SM_X, S_SM_Y, fg2, G_DOT);
            snprintf(col, sizeof(col), "%d ITEMS", it->n_items);
            ui_text(c, x_len - ui_text_w(col, S_SM_X), y + 2, S_SM_X,
                    S_SM_Y, fg2, col);
            if (it->size) {
                fmt_size(it->size, col, sizeof(col));
                ui_text(c, x_sz - ui_text_w(col, S_SM_X), y + 2, S_SM_X,
                        S_SM_Y, fg2, col);
            }
        } else if (!it->is_dir) {
            if (it->scanned && !it->scan_failed) {
                ui_text(c, x_std, y + 2, S_SM_X, S_SM_Y, fg2,
                        it->standard ? "PAL" : "NTSC");
                ui_text(c, x_asp, y + 2, S_SM_X, S_SM_Y, fg2,
                        it->wide ? "16:9" : "4:3");
                fmt_dur_short(it->longest, col, sizeof(col));
                ui_text(c, x_len - ui_text_w(col, S_SM_X), y + 2, S_SM_X,
                        S_SM_Y, fg2, col);
            } else {
                ui_text(c, x_std, y + 2, S_SM_X, S_SM_Y, fg2,
                        it->scanned ? "?" : spinner(v->tick + idx));
            }
            fmt_size(it->size, col, sizeof(col));
            ui_text(c, x_sz - ui_text_w(col, S_SM_X), y + 2, S_SM_X,
                    S_SM_Y, fg2, col);
        }
    }

    /* selected-disc detail strip */
    int sy = g.y1 - 46;
    ui_hline2(c, g.x0, sy - 6, g.x1 - g.x0, th->dim);
    if (v->sel >= 0 && v->sel < v->n_items) {
        const ui_item_t *it = v->items[v->sel];
        if (!it->is_dir && it->scanned && !it->scan_failed) {
            char reg[32], tt[24], au[64] = "", su[80] = "";
            fmt_region(it->region_mask, reg, sizeof(reg));
            snprintf(tt, sizeof(tt), "%d TITLES", it->titles);
            if (it->n_audio)
                snprintf(au, sizeof(au), "%s%s%s", it->audio[0],
                         it->n_audio > 1 ? " +" : "",
                         it->n_audio > 1 ? "" : "");
            if (it->subs[0])
                snprintf(su, sizeof(su), "SUBS %s", it->subs);
            const char *f[5] = { it->name, reg, tt, au[0] ? au : 0,
                                 su[0] ? su : 0 };
            strip(c, g.x0 + 8, sy, S_SM_X, S_SM_Y, th->bright, th->dim, f,
                  5, g.x1 - 8);
        }
    }

    hint_t h[5] = {
        { G_UP G_DOWN, "SELECT" },
        { G_ENTER, "PLAY" },
        { v->at_root ? G_GEAR : G_BACK, v->at_root ? "SETTINGS" : "BACK" },
        { G_LEFT " " G_RIGHT, "PAGE" },
        { G_STOP, "EJECT" },
    };
    footer(c, &g, th, h, 5, false);
}

/* ---- SETTINGS ----------------------------------------------------------- */

static void render_settings(ui_canvas_t *c, const ui_view_t *v,
                            const ui_theme_t *th)
{
    geo_t g = geo(c);
    int x = ui_text(c, g.x0 + 24, g.y0 + 8, S_LS_X, S_LS_Y, th->hot,
                    G_DISC);
    ui_text(c, x + 8, g.y0 + 8, S_LS_X, S_LS_Y, th->bright, "SETTINGS");

    int xl = g.x0 + 56, xv = g.x0 + 320;
    int y = g.y0 + 76, pitch = 36;
    uint32_t off = ui_lerp(th->bg, th->dim, 110);  /* disabled: fainter than dim */
    for (int r = 0; r < UI_SET_ROWS; r++, y += pitch) {
        bool sel = (r == v->set_sel);
        bool edit = sel && v->set_editing;   /* row is open for adjustment */
        bool on = ui_settings_enabled(v->set, r);
        ui_text(c, xl, y, S_LS_X, S_LS_Y,
                !on ? off : (sel ? th->text : th->dim), ui_settings_label(r));
        char val[64];
        if (edit)   /* ‹ › arrows appear only while the row is being adjusted */
            snprintf(val, sizeof(val), G_LEFT " %s " G_RIGHT,
                     ui_settings_value(v->set, r));
        else
            snprintf(val, sizeof(val), "%s", ui_settings_value(v->set, r));
        if (sel) {
            int w = ui_text_w(val, S_LS_X);
            /* armed/editing row glows in the hot accent; a merely-selected row
             * sits in the calm bar so the two states never read alike. */
            ui_fill(c, xv - 8, y - 3, w + 16, 22, edit ? th->hot : th->bar);
            ui_text(c, xv, y, S_LS_X, S_LS_Y, edit ? th->bg : th->bartxt, val);
        } else {
            ui_text(c, xv, y, S_LS_X, S_LS_Y, !on ? off : th->bright, val);
        }
    }

    const char *ver = "PIDVD 0.4 " G_DOT " 15 kHz " G_DOT
                      " CRT NEVER LIES";
    ui_text(c, xl, g.y1 - 64, S_SM_X, S_SM_Y, th->dim, ver);

    hint_t h[3];
    if (v->set_editing) {
        h[0] = (hint_t){ G_LEFT G_RIGHT, "CHANGE" };
        h[1] = (hint_t){ G_ENTER, "CONFIRM" };
        h[2] = (hint_t){ G_BACK, "REVERT" };   /* Back undoes the edit */
    } else {
        h[0] = (hint_t){ G_UP G_DOWN, "SELECT" };
        h[1] = (hint_t){ G_ENTER, "EDIT" };
        h[2] = (hint_t){ G_BACK, "CLOSE" };    /* Back leaves SETTINGS */
    }
    footer(c, &g, th, h, 3, false);
}

/* ---- entry -------------------------------------------------------------- */

int pidvd_ui_visible_rows(const ui_view_t *v, int canvas_h)
{
    int y0 = (canvas_h >= 576) ? 38 : 32;
    int yb = y0 + 34 + (v->now_playing ? 28 : 0);
    if (v->set->layout == UI_LEDGER)
        return (canvas_h - y0 - 52 - (yb + LED_ROW_H + 4)) / LED_ROW_H;
    if (v->set->layout == UI_MARQUEE)
        return 1;
    return (canvas_h - y0 - 26 - yb) / CON_ROW_H;
}

void pidvd_ui_render(ui_canvas_t *c, const ui_view_t *v)
{
    int ti = v->set->theme;
    if (ti < 0 || ti >= PIDVD_N_THEMES)
        ti = 0;
    const ui_theme_t *th = &pidvd_themes[ti];
    ui_fill(c, 0, 0, c->w, c->h, th->bg);

    /* Idle screensaver paints over whatever screen is underneath; the run
     * loop keeps that screen current so it's right the instant we wake. */
    if (v->saver_active) {
        pidvd_saver_render(c, th, v->set->saver, (unsigned)v->tick);
        return;
    }

    switch (v->screen) {
    case UI_ATTRACT:
        render_attract(c, v, th);
        break;
    case UI_SETTINGS:
        render_settings(c, v, th);
        break;
    case UI_BROWSE:
        switch ((ui_layout_t)v->set->layout) {
        case UI_MARQUEE:   render_marquee(c, v, th);   break;
        case UI_LEDGER:    render_ledger(c, v, th);    break;
        case UI_WIREFRAME: render_wireframe(c, v, th); break;
        case UI_CONSOLE:
        default:           render_console(c, v, th);   break;
        }
        break;
    }
}
