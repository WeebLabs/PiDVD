/* Warp starfield screensaver — a deep, perspective-correct star tunnel, the
 * Wrath-of-Khan title look: faint stars massed at the vanishing point that
 * brighten, fatten and streak as they tear past the camera. Pure pixel code;
 * see saver.h.
 *
 * The depth is REAL, not a radial dimming trick: each star is a fixed point
 * (ox, oy, z) in a volume ahead of the camera, and we project it the honest
 * way — screen = centre + (ox, oy)·focal/z. Everything else (acceleration,
 * brightening, streak growth, thickening) falls out of 1/z. A faint static
 * layer of distant stars sits behind the warp so the void has depth too.
 *
 * It is deliberately STATELESS: there is no array of star state updated per
 * frame. Each star is a fixed seed (lateral offset + phase, hashed from its
 * index) and its z at field `tick` is a single global warp phase. So frame N
 * depends only on (tick, theme, canvas size) — exactly render.c's contract —
 * and nudging any tunable below shows up on the very next field, no warm-up.
 *
 * Geometry is authored in the picker's logical 720x480 space (decimated 2:1
 * to 240p on the CRT); thicknesses are kept even so they survive that
 * decimation cleanly (interlace law, docs/UI.md §1). */
#include "ui/saver.h"

#include <math.h>
#include <stddef.h>

#include "ui/dvdlogo_data.h"   /* embedded DVD-Video logo alpha mask */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ======================== WARP STARFIELD TUNABLES =======================
 * The whole look lives here. Depths (z) are in arbitrary camera units;
 * lengths in logical (720x480) pixels; rates in Hz. Set any flourish's
 * amplitude to 0 to switch it off.
 */
#define WARP_BG          0x000000u /* backdrop colour: space is black whatever *
                                    * the UI theme is (stars keep theme tones) */

/* --- the warp field (true 1/z perspective) ---------------------------- */
#define WARP_STARS       280     /* stars streaming at once — density of field*/
#define WARP_RATE        0.0090f /* z covered per field (fraction of z-range); *
                                  * UP = faster warp                          */
#define WARP_ZNEAR       0.16f   /* closest z before a star recycles; smaller *
                                  * = it whips further past you at the end    */
#define WARP_ZFAR        5.0f    /* spawn depth; bigger = deeper field, fainter*
                                  * and more crowded at the vanishing point   */
#define WARP_FOCAL       0.95f   /* lens: screen radius = offset·FOCAL·halfmin *
                                  * /z. UP = field spreads/exits sooner       */
#define WARP_SPREAD      1.00f   /* lateral spread of the star tube; UP pushes *
                                  * stars wider, fewer aimed straight at you   */
#define WARP_STREAK      0.045f  /* comet length: z-phase span behind the head;*
                                  * 1/z makes it short far off, long up close  */
#define WARP_BRIGHT_Z    0.70f   /* z at/under which a star is fully bright;   *
                                  * larger = stars light up earlier/deeper     */
#define WARP_MAG_VAR     0.45f   /* per-star brightness spread (0 = uniform):  *
                                  * real fields have dim and brilliant stars   */
#define WARP_THICK       2       /* min streak thickness, px (even for clean   *
                                  * 2:1 decimation to 240p)                    */
#define WARP_THICK_NEAR  6       /* max head thickness up close, px (even)     */
#define WARP_THICK_Z     2.2f    /* z·thickness constant: thickness = K/z      *
                                  * clamped to [THICK, THICK_NEAR]             */

/* --- distant backdrop: faint static stars for depth behind the warp ---- */
#define WARP_DUST        150     /* count of background stars (0 disables)     */
#define WARP_DUST_AMT    150     /* peak backdrop brightness, 0..256 of `dim`  */
#define WARP_DUST_TWHZ   0.20f   /* twinkle rate (Hz); 0 = steady             */

/* --- motion flourishes (amplitude 0 disables each) --------------------- */
#define WARP_ROLL        0.035f  /* whole-field roll (rad/s): a gentle bank    */
#define WARP_DRIFT_X     26.0f   /* the vanishing point wanders a Lissajous    */
#define WARP_DRIFT_Y     16.0f   /* path — amplitude (px) ...                  */
#define WARP_DRIFT_HZ_X  0.013f  /* ... and rate (Hz) per axis, so the         */
#define WARP_DRIFT_HZ_Y  0.019f  /* "ship" feels like it's banking             */
#define WARP_PULSE_AMT   0.30f   /* warp-speed breathing: 0 = constant speed,  */
#define WARP_PULSE_HZ    0.040f  /* UP = surges into and out of hyperspace     */

/* --- star character ---------------------------------------------------- */
#define WARP_HERO_EVERY  11      /* every Nth star burns to `hot` and runs fat */
#define WARP_HERO_THICK  2       /* extra px of thickness for a hero star      */
/* ======================================================================== */

/* ======================== DVD BOUNCE TUNABLES ===========================
 * The classic "waiting for it to hit the corner" logo. Stateless: position
 * is a triangle wave of the field counter, so the bounce is exact and the
 * colour-per-bounce is just a function of how many reflections have happened.
 * Speeds are px/second; lengths px; angles handled via the two axis speeds.
 */
#define DVD_SCALE       1.00f   /* logo size multiplier (asset is 184x110 px) */
#define DVD_SPEED_X     58.0f   /* horizontal drift, px/s (slow = nostalgic)  */
#define DVD_SPEED_Y     43.0f   /* vertical drift, px/s; different from X so  *
                                 * the path precesses and eventually corners  */
#define DVD_INSET       6       /* px the bounce box is inset from the screen */
#define DVD_LOGOS       1       /* how many logos bounce at once (each offset) */
#define DVD_ALPHA_MIN   20      /* skip near-transparent mask pixels          */

/* Colour. DVD_COLOR_MODE: 0 THEME (cycle the active theme's tones), 1 CLASSIC
 * (the nostalgic vivid palette), 2 RAINBOW (hue steps each bounce), 3 FIXED. */
#define DVD_COLOR_MODE  0
#define DVD_FIXED_COLOR 0xF4EFE2u /* used when DVD_COLOR_MODE == 3            */
#define DVD_RAINBOW_DEG 47.0f     /* hue advance per bounce, degrees (mode 2) */
#define DVD_RAINBOW_SAT 0.85f
#define DVD_RAINBOW_VAL 1.00f

#define DVD_BG          0x000000u /* backdrop behind the logo                 */
#define DVD_SHADOW      0         /* drop-shadow offset px (0 = no shadow)    */
#define DVD_SHADOW_AMT  150       /* shadow darkness, 0..256                  */

/* Corner celebration: when the logo nails a corner, optionally wash the
 * background with a faint tint for that moment. 0 disables. */
#define DVD_CORNER_FLASH 1
#define DVD_CORNER_AMT   40       /* flash strength, 0..256 of the logo tint  */
/* ======================================================================== */

#define HZ   ((float)PIDVD_SAVER_FIELD_HZ)
#define TAU  (2.0f * (float)M_PI)
#define ZRANGE (WARP_ZFAR - WARP_ZNEAR)

/* fmix32 (a la SplitMix) — a fixed, deterministic scramble of a star index */
static inline uint32_t hash_u32(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}
static inline float hash01(uint32_t x)   /* deterministic 0..1 from a seed */
{
    return (float)(hash_u32(x) >> 8) * (1.0f / 16777216.0f);
}
static inline float fracf(float x) { return x - floorf(x); }
static inline float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

static inline void plot(ui_canvas_t *c, float x, float y, int thick,
                        uint32_t col)
{
    /* ui_fill clips, so off-screen blocks (stars past the edge) are free */
    ui_fill(c, (int)x - thick / 2, (int)y - thick / 2, thick, thick, col);
}

/* brightness 0..1 -> colour: void -> dim -> bright (hero: -> hot). The lift
 * off WARP_BG at the low end is what makes far stars fade in, not pop. */
static uint32_t warp_color(const ui_theme_t *th, float b, int hero)
{
    int t = (int)(clamp01(b) * 256.0f);
    uint32_t lo = ui_lerp(WARP_BG, th->dim, t < 256 ? t : 256);
    uint32_t hi = hero ? th->hot : th->bright;
    return ui_lerp(lo, hi, t);   /* near unity -> the hot/bright head tone */
}

/* perspective thickness K/z, clamped even, plus the hero bonus */
static inline int warp_thick(float z, int hero)
{
    int t = (int)(WARP_THICK_Z / z + 0.5f);
    if (t < WARP_THICK)      t = WARP_THICK;
    if (t > WARP_THICK_NEAR) t = WARP_THICK_NEAR;
    t -= (t & 1);
    if (t < 2) t = 2;
    return t + (hero ? WARP_HERO_THICK : 0);
}

/* The faint, near-static deep-space backdrop drawn behind the warp. */
static void render_dust(ui_canvas_t *c, const ui_theme_t *th, float t)
{
    for (int i = 0; i < WARP_DUST; i++) {
        float x = hash01((uint32_t)i * 3u + 11u) * (float)c->w;
        float y = hash01((uint32_t)i * 3u + 23u) * (float)c->h;
        float ph = hash01((uint32_t)i * 3u + 37u);
        float tw = 1.0f;
        if (WARP_DUST_TWHZ > 0.0f)   /* gentle shimmer, each star out of phase */
            tw = 0.55f + 0.45f * sinf(TAU * (WARP_DUST_TWHZ * t / HZ + ph));
        int amt = (int)(WARP_DUST_AMT * (0.4f + 0.6f * ph) * tw);
        plot(c, x, y, 2, ui_lerp(WARP_BG, th->dim, amt));
    }
}

static void render_warp(ui_canvas_t *c, const ui_theme_t *th, unsigned tick)
{
    ui_fill(c, 0, 0, c->w, c->h, WARP_BG);   /* black void over the theme bg */

    float t = (float)tick;
    if (WARP_DUST > 0)
        render_dust(c, th, t);

    float halfmin = 0.5f * (float)(c->w < c->h ? c->w : c->h);
    float focal   = WARP_FOCAL * halfmin;

    /* Continuous warp phase Φ(t) = ∫ rate dt for a breathing rate
     * R·(1 + A·sin(ω t)). The closed-form integral (not t·rate(t)) keeps the
     * z-stream seamless when the pulse changes speed. */
    float wrad  = TAU * WARP_PULSE_HZ / HZ;
    float phase = WARP_RATE * t;
    if (WARP_PULSE_AMT != 0.0f && wrad != 0.0f)
        phase += (WARP_RATE * WARP_PULSE_AMT / wrad) * (1.0f - cosf(wrad * t));

    float roll = WARP_ROLL * t / HZ;
    float cr = cosf(roll), sr = sinf(roll);            /* field bank/roll */
    float cx = (float)c->w * 0.5f
             + WARP_DRIFT_X * sinf(TAU * WARP_DRIFT_HZ_X * t / HZ);
    float cy = (float)c->h * 0.5f
             + WARP_DRIFT_Y * sinf(TAU * WARP_DRIFT_HZ_Y * t / HZ);

    for (int i = 0; i < WARP_STARS; i++) {
        /* fixed lateral offset in a square cross-section [-SPREAD,SPREAD]^2,
         * rolled by the field bank; near-zero offset = aimed at the camera */
        float ux = (hash01((uint32_t)i * 4u + 1u) - 0.5f) * 2.0f * WARP_SPREAD;
        float uy = (hash01((uint32_t)i * 4u + 2u) - 0.5f) * 2.0f * WARP_SPREAD;
        float ox = ux * cr - uy * sr;
        float oy = ux * sr + uy * cr;
        float off = hash01((uint32_t)i * 4u + 3u);
        float mag = 1.0f - WARP_MAG_VAR * hash01((uint32_t)i * 4u + 5u);
        int   hero = (WARP_HERO_EVERY > 0) && (i % WARP_HERO_EVERY == 0);

        /* z streams from ZFAR down to ZNEAR, then wraps (off-screen, so the
         * recycle is invisible). Streak tail sits a little further back. */
        float fr_head = fracf(off + phase);
        float fr_tail = fr_head - WARP_STREAK;
        if (fr_tail < 0.0f) fr_tail = 0.0f;
        float z_head = WARP_ZFAR - fr_head * ZRANGE;
        float z_tail = WARP_ZFAR - fr_tail * ZRANGE;

        float kh = focal / z_head, kt = focal / z_tail;
        float hx = cx + ox * kh, hy = cy + oy * kh;     /* head (near)  */
        float tx = cx + ox * kt, ty = cy + oy * kt;     /* tail (far)   */

        float b_head = clamp01(WARP_BRIGHT_Z / z_head) * mag;
        float b_tail = clamp01(WARP_BRIGHT_Z / z_tail) * mag;
        int   th_head = warp_thick(z_head, hero);
        int   th_tail = warp_thick(z_tail, hero);

        /* Lay the comet as blocks from tail to head — evenly in screen space
         * so there are no gaps — brightening and fattening toward the head. */
        float dx = hx - tx, dy = hy - ty;
        float len = sqrtf(dx * dx + dy * dy);
        int steps = (int)(len * 0.5f) + 1;              /* ~one per 2 px: solid*/
        if (steps > 140) steps = 140;
        for (int s = 0; s <= steps; s++) {
            float f = steps ? (float)s / (float)steps : 1.0f;
            float b = b_tail + (b_head - b_tail) * f;
            int thk = (int)(th_tail + (th_head - th_tail) * f + 0.5f);
            thk -= (thk & 1);
            if (thk < 2) thk = 2;
            plot(c, tx + dx * f, ty + dy * f, thk, warp_color(th, b, hero));
        }
    }
}

/* ----------------------------- DVD bounce ------------------------------ */

static inline void putpx(ui_canvas_t *c, int x, int y, uint32_t col)
{
    if ((unsigned)x < (unsigned)c->w && (unsigned)y < (unsigned)c->h)
        *((uint32_t *)(c->px + (size_t)y * c->stride) + x) = col;
}

/* reflect p into [0,range] — the bounce as a triangle wave */
static float tri(float p, float range)
{
    float per = 2.0f * range;
    float m = fmodf(p, per);
    if (m < 0.0f) m += per;
    return m <= range ? m : per - m;
}

static uint32_t hsv(float h, float s, float v)   /* h deg, s/v 0..1 -> XRGB */
{
    h = fmodf(h, 360.0f); if (h < 0.0f) h += 360.0f;
    float c1 = v * s, x = c1 * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
    float m = v - c1, r = 0, g = 0, b = 0;
    if      (h <  60) { r = c1; g = x;  }
    else if (h < 120) { r = x;  g = c1; }
    else if (h < 180) { g = c1; b = x;  }
    else if (h < 240) { g = x;  b = c1; }
    else if (h < 300) { r = x;  b = c1; }
    else              { r = c1; b = x;  }
    uint32_t R = (uint32_t)((r + m) * 255.0f + 0.5f);
    uint32_t G = (uint32_t)((g + m) * 255.0f + 0.5f);
    uint32_t B = (uint32_t)((b + m) * 255.0f + 0.5f);
    return (R << 16) | (G << 8) | B;
}

/* the nostalgic multicolour cycle (mode 1) */
static const uint32_t dvd_classic[] = {
    0xFF3030u, 0xFF9020u, 0xF0E000u, 0x40E040u,
    0x30D0E0u, 0x4060FFu, 0xC050FFu, 0xF8F8F8u,
};

/* tint for the logo after `n` total bounces */
static uint32_t dvd_tint(const ui_theme_t *th, int n)
{
    int k = ((n % 8) + 8) % 8;
    switch (DVD_COLOR_MODE) {
    case 1: return dvd_classic[k];
    case 2: return hsv(n * DVD_RAINBOW_DEG, DVD_RAINBOW_SAT, DVD_RAINBOW_VAL);
    case 3: return DVD_FIXED_COLOR;
    default: {                              /* 0: cycle the theme's tones */
        const uint32_t pal[4] = { th->bright, th->hot, th->text, th->bar };
        return pal[k & 3];
    }
    }
}

/* nearest-neighbour blit of the logo mask, tinted, blended over bg */
static void blit_logo(ui_canvas_t *c, int x0, int y0, int dw, int dh,
                      uint32_t tint, uint32_t bg)
{
    for (int dy = 0; dy < dh; dy++) {
        int yy = y0 + dy;
        if ((unsigned)yy >= (unsigned)c->h) continue;
        int sy = dy * PIDVD_DVDLOGO_H / dh;
        const uint8_t *row = &pidvd_dvdlogo_a[sy * PIDVD_DVDLOGO_W];
        for (int dx = 0; dx < dw; dx++) {
            int a = row[dx * PIDVD_DVDLOGO_W / dw];
            if (a < DVD_ALPHA_MIN) continue;
            putpx(c, x0 + dx, yy, a >= 250 ? tint : ui_lerp(bg, tint, a));
        }
    }
}

static void render_dvd(ui_canvas_t *c, const ui_theme_t *th, unsigned tick)
{
    float t = (float)tick;
    int   dw = (int)(PIDVD_DVDLOGO_W * DVD_SCALE + 0.5f);
    int   dh = (int)(PIDVD_DVDLOGO_H * DVD_SCALE + 0.5f);
    float rx = (float)(c->w - 2 * DVD_INSET - dw);
    float ry = (float)(c->h - 2 * DVD_INSET - dh);
    if (rx < 1.0f) rx = 1.0f;
    if (ry < 1.0f) ry = 1.0f;
    float vx = DVD_SPEED_X / HZ, vy = DVD_SPEED_Y / HZ;

    /* First pass: find any corner hit so the backdrop can react before the
     * logos are drawn over it. */
    uint32_t bg = DVD_BG;
    for (int l = 0; l < DVD_LOGOS && DVD_CORNER_FLASH; l++) {
        float px = rx * fracf(0.13f + 0.37f * l) + vx * t;
        float py = ry * fracf(0.29f + 0.53f * l) + vy * t;
        float x = tri(px, rx), y = tri(py, ry);
        float dxw = x < rx - x ? x : rx - x;        /* dist to nearest x wall */
        float dyw = y < ry - y ? y : ry - y;
        if (dxw <= fabsf(vx) + 0.5f && dyw <= fabsf(vy) + 0.5f) {
            int n = (int)floorf(px / rx) + (int)floorf(py / ry) + l;
            bg = ui_lerp(DVD_BG, dvd_tint(th, n), DVD_CORNER_AMT);
            break;
        }
    }
    ui_fill(c, 0, 0, c->w, c->h, bg);

    for (int l = 0; l < DVD_LOGOS; l++) {
        float px = rx * fracf(0.13f + 0.37f * l) + vx * t;
        float py = ry * fracf(0.29f + 0.53f * l) + vy * t;
        int   n  = (int)floorf(px / rx) + (int)floorf(py / ry) + l;
        int   x0 = DVD_INSET + (int)(tri(px, rx) + 0.5f);
        int   y0 = DVD_INSET + (int)(tri(py, ry) + 0.5f);
        uint32_t tint = dvd_tint(th, n);
        if (DVD_SHADOW)
            blit_logo(c, x0 + DVD_SHADOW, y0 + DVD_SHADOW, dw, dh,
                      ui_lerp(bg, 0x000000u, DVD_SHADOW_AMT), bg);
        blit_logo(c, x0, y0, dw, dh, tint, bg);
    }
}

void pidvd_saver_render(ui_canvas_t *c, const ui_theme_t *th,
                        int kind, unsigned tick)
{
    switch (kind) {
    case PIDVD_SAVER_WARP:
        render_warp(c, th, tick);
        break;
    case PIDVD_SAVER_DVD:
        render_dvd(c, th, tick);
        break;
    case PIDVD_SAVER_OFF:
    default:
        break;   /* canvas is already the theme background */
    }
}
