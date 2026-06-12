#include "decode/spu.h"

#include <stdlib.h>
#include <string.h>

#define SPU_MAX 65536
#define OVL_W 720
#define OVL_H 576

struct pidvd_spu {
    int stream;            /* selected substream, -1 = off */
    /* packet assembly */
    uint8_t buf[SPU_MAX];
    size_t have, want;
    /* decoded state */
    uint8_t idx[OVL_W * OVL_H];  /* 2-bit color index per pixel */
    int x, y, w, h;
    bool visible;
    uint8_t pal[4], alpha[4];    /* base palette (CLUT idx) + alpha 0-15 */
    /* highlight */
    bool hl;
    int hl_sx, hl_sy, hl_ex, hl_ey;
    uint8_t hl_pal[4], hl_alpha[4];
    /* output */
    uint32_t clut_rgba[16];      /* CLUT converted to RGB */
    uint8_t *rgba;
    bool dirty;
};

pidvd_spu_t *pidvd_spu_new(void)
{
    pidvd_spu_t *s = calloc(1, sizeof(*s));
    s->stream = -1;
    s->rgba = malloc(OVL_W * OVL_H * 4);
    return s;
}

void pidvd_spu_free(pidvd_spu_t *s)
{
    if (!s)
        return;
    free(s->rgba);
    free(s);
}

void pidvd_spu_set_clut(pidvd_spu_t *s, const uint32_t clut[16])
{
    for (int i = 0; i < 16; i++) {
        /* IFO palette entry: 0x00YYCrCb, BT.601 */
        int y  = (clut[i] >> 16) & 0xff;
        int cr = (clut[i] >> 8) & 0xff;
        int cb = clut[i] & 0xff;
        int r = y + (1.402 * (cr - 128));
        int g = y - (0.344136 * (cb - 128)) - (0.714136 * (cr - 128));
        int b = y + (1.772 * (cb - 128));
        r = r < 0 ? 0 : (r > 255 ? 255 : r);
        g = g < 0 ? 0 : (g > 255 ? 255 : g);
        b = b < 0 ? 0 : (b > 255 ? 255 : b);
        /* match scanout byte order (B,G,R,X little-endian) */
        s->clut_rgba[i] = (uint32_t)b | ((uint32_t)g << 8)
                        | ((uint32_t)r << 16);
    }
    s->dirty = true;
}

void pidvd_spu_select_stream(pidvd_spu_t *s, int substream)
{
    if (s->stream != substream) {
        s->stream = substream;
        s->have = s->want = 0;
        s->visible = false;
        s->dirty = true;
    }
}

void pidvd_spu_clear(pidvd_spu_t *s)
{
    s->have = s->want = 0;
    s->visible = false;
    s->hl = false;
    s->dirty = true;
}

/* --- RLE ------------------------------------------------------------ */

struct bits {
    const uint8_t *d;
    size_t len, pos;       /* pos in nibbles */
};

static int nib(struct bits *b)
{
    if (b->pos / 2 >= b->len)
        return -1;
    uint8_t v = b->d[b->pos / 2];
    return (b->pos++ & 1) ? (v & 0xf) : (v >> 4);
}

static void decode_field(pidvd_spu_t *s, size_t off, int field)
{
    struct bits b = { .d = s->buf, .len = s->have, .pos = off * 2 };
    for (int y = field; y < s->h; y += 2) {
        int x = 0;
        while (x < s->w) {
            int v = nib(&b);
            if (v < 0)
                return;
            if (v < 0x4) {
                v = (v << 4) | nib(&b);
                if (v < 0x10) {
                    v = (v << 4) | nib(&b);
                    if (v < 0x40)
                        v = (v << 4) | nib(&b);
                }
            }
            int count = v >> 2;
            int color = v & 3;
            if (count == 0 || count > s->w - x)
                count = s->w - x;
            memset(&s->idx[(size_t)y * s->w + x], color, count);
            x += count;
        }
        b.pos = (b.pos + 1) & ~1u;   /* byte-align per line */
    }
}

/* --- control sequence ------------------------------------------------ */

static void parse_spu(pidvd_spu_t *s)
{
    if (s->have < 4)
        return;
    size_t dcsq = ((size_t)s->buf[2] << 8) | s->buf[3];
    size_t rle0 = 0, rle1 = 0;
    bool show = false;

    /* execute the first control sequence (delay 0); later sequences
     * carry timed stop commands — handled when the PTS clock exists.
     * TODO(m3): honor SP_DCSQ delays for subtitle durations. */
    if (dcsq + 4 > s->have)
        return;
    size_t p = dcsq + 4;          /* skip delay + next-offset */
    bool done = false;
    while (p < s->have && !done) {
        switch (s->buf[p++]) {
        case 0x00:                /* forced display (menus) */
        case 0x01:                /* start display */
            show = true;
            break;
        case 0x02:                /* stop display */
            show = false;
            break;
        case 0x03:                /* SET_COLOR */
            if (p + 2 > s->have) return;
            s->pal[3] = s->buf[p] >> 4;
            s->pal[2] = s->buf[p] & 0xf;
            s->pal[1] = s->buf[p + 1] >> 4;
            s->pal[0] = s->buf[p + 1] & 0xf;
            p += 2;
            break;
        case 0x04:                /* SET_CONTR (alpha) */
            if (p + 2 > s->have) return;
            s->alpha[3] = s->buf[p] >> 4;
            s->alpha[2] = s->buf[p] & 0xf;
            s->alpha[1] = s->buf[p + 1] >> 4;
            s->alpha[0] = s->buf[p + 1] & 0xf;
            p += 2;
            break;
        case 0x05: {              /* SET_DAREA */
            if (p + 6 > s->have) return;
            int x1 = (s->buf[p] << 4) | (s->buf[p + 1] >> 4);
            int x2 = ((s->buf[p + 1] & 0xf) << 8) | s->buf[p + 2];
            int y1 = (s->buf[p + 3] << 4) | (s->buf[p + 4] >> 4);
            int y2 = ((s->buf[p + 4] & 0xf) << 8) | s->buf[p + 5];
            s->x = x1; s->y = y1;
            s->w = x2 - x1 + 1;
            s->h = y2 - y1 + 1;
            if (s->w > OVL_W) s->w = OVL_W;
            if (s->h > OVL_H) s->h = OVL_H;
            p += 6;
            break;
        }
        case 0x06:                /* SET_DSPXA */
            if (p + 4 > s->have) return;
            rle0 = ((size_t)s->buf[p] << 8) | s->buf[p + 1];
            rle1 = ((size_t)s->buf[p + 2] << 8) | s->buf[p + 3];
            p += 4;
            break;
        case 0x07:                /* CHG_COLCON — rare, skip payload */
            if (p + 2 > s->have) return;
            p += (((size_t)s->buf[p] << 8) | s->buf[p + 1]) - 1;
            break;
        case 0xff:
            done = true;
            break;
        default:
            done = true;
            break;
        }
    }

    if (s->w > 0 && s->h > 0 && rle0 && rle1) {
        memset(s->idx, 0, (size_t)s->w * s->h);
        decode_field(s, rle0, 0);
        decode_field(s, rle1, 1);
        s->visible = show;
        s->dirty = true;
    }
}

void pidvd_spu_packet(pidvd_spu_t *s, int substream,
                      const uint8_t *data, size_t len)
{
    if (substream != s->stream || len == 0)
        return;
    if (s->have == 0) {
        if (len < 2)
            return;
        s->want = ((size_t)data[0] << 8) | data[1];
        if (s->want > SPU_MAX)
            s->want = 0;
    }
    if (s->have + len > SPU_MAX)
        { s->have = s->want = 0; return; }
    memcpy(s->buf + s->have, data, len);
    s->have += len;
    if (s->want && s->have >= s->want) {
        parse_spu(s);
        s->have = s->want = 0;
    }
}

/* --- highlight + render ---------------------------------------------- */

bool pidvd_spu_set_highlight(pidvd_spu_t *s, int sx, int sy, int ex,
                             int ey, uint32_t palette)
{
    s->hl = true;
    s->hl_sx = sx; s->hl_sy = sy; s->hl_ex = ex; s->hl_ey = ey;
    for (int i = 0; i < 4; i++) {
        s->hl_pal[i]   = (palette >> (16 + 4 * i)) & 0xf;
        s->hl_alpha[i] = (palette >> (4 * i)) & 0xf;
    }
    s->dirty = true;
    return s->visible;
}

bool pidvd_spu_clear_highlight(pidvd_spu_t *s)
{
    if (!s->hl)
        return false;
    s->hl = false;
    s->dirty = true;
    return s->visible;
}

bool pidvd_spu_overlay(pidvd_spu_t *s, pidvd_overlay_t *out)
{
    if (!s->visible || s->w <= 0 || s->h <= 0)
        return false;
    if (s->dirty) {
        for (int y = 0; y < s->h; y++) {
            int abs_y = s->y + y;
            for (int x = 0; x < s->w; x++) {
                uint8_t ci = s->idx[(size_t)y * s->w + x];
                int abs_x = s->x + x;
                bool in_hl = s->hl
                    && abs_x >= s->hl_sx && abs_x <= s->hl_ex
                    && abs_y >= s->hl_sy && abs_y <= s->hl_ey;
                uint8_t pal = in_hl ? s->hl_pal[ci] : s->pal[ci];
                uint8_t a4  = in_hl ? s->hl_alpha[ci] : s->alpha[ci];
                uint32_t rgb = s->clut_rgba[pal];
                uint32_t px = rgb | ((uint32_t)(a4 * 17) << 24);
                memcpy(&s->rgba[((size_t)y * s->w + x) * 4], &px, 4);
            }
        }
        s->dirty = false;
    }
    out->x = s->x;
    out->y = s->y;
    out->w = s->w;
    out->h = s->h;
    out->rgba = s->rgba;
    return true;
}
