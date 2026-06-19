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

void pidvd_saver_render(ui_canvas_t *c, const ui_theme_t *th,
                        int kind, unsigned tick)
{
    switch (kind) {
    case PIDVD_SAVER_WARP:
        render_warp(c, th, tick);
        break;
    case PIDVD_SAVER_OFF:
    default:
        break;   /* canvas is already the theme background */
    }
}
