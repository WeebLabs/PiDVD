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

/* ========================= FIREWORKS TUNABLES ===========================
 * A night sky over which shells rise, burst and rain down. It is STATELESS
 * like the warp field: there is no array of live particles. Time is a stream
 * of shells indexed by an integer k — shell k is launched at ~k/RATE seconds,
 * and EVERY property of it (where it rises from, how high it bursts, its
 * colour, type, spark directions, lifetimes) is a deterministic hash of k (and
 * of the spark index j). So a given field `tick` re-derives exactly the shells
 * that should be alive then, and tweaking any knob below shows up on the very
 * next field with no warm-up — render.c's contract.
 *
 * Each spark flies a closed-form trajectory under gravity + air drag
 *     v(t) = v_ss + (v0 - v_ss)e^{-DRAG t},  v_ss = GRAVITY/DRAG  (terminal)
 *     p(t) = p0 + v_ss t + (v0 - v_ss)(1 - e^{-DRAG t})/DRAG
 * evaluated directly at the burst-age — no integration over frames. Initial
 * velocities are a random point on a 3-D unit sphere (the z-component shades
 * brightness/size for depth), so a peony reads as a sphere, not a flat disc.
 *
 * Lengths are fractions of canvas HEIGHT so the look holds at any resolution;
 * thicknesses stay even for clean 2:1 decimation to 240p (interlace law). Set
 * any amplitude to 0 to switch a flourish off.
 */

/* --- the night sky ----------------------------------------------------- */
#define FW_BG            0x000000u /* sky colour; black whatever the UI theme */
#define FW_BG_THEME      0         /* 1: tint the sky toward the theme bg      */
#define FW_BG_THEME_AMT  60        /* 0..256 blend toward th->bg if FW_BG_THEME*/

/* --- COLOUR: the switch you asked for --------------------------------- */
#define FW_THEME_COLORS  0         /* 1: hues from the ACTIVE THEME;           *
                                    * 0: traditional firework colours (below)  */
#define FW_THEME_BLEND   1         /* theme mode: 1 blend smoothly across the  *
                                    * theme's tones for variety, 0 pick discrete*/
#define FW_MULTICOLOR        1     /* 1: some shells give each spark its own   *
                                    * colour (a "brocade"); 0: mono shells     */
#define FW_MULTICOLOR_CHANCE 0.22f /* fraction of shells that are multicolour  */

/* --- launch cadence ---------------------------------------------------- */
#define FW_RATE          2.4f      /* shells launched per second               */
#define FW_RATE_VAR      0.45f     /* 0..1 jitter on the interval (no metronome)*/
#define FW_MAX_SHELLS    22        /* most shells drawn at once (density/perf)  */

/* --- depth / parallax: shells at varying distance from the camera ------ *
 * Each shell gets a hashed distance. Near (foreground) shells are larger,
 * brighter and thicker and are painted last so they layer over the far ones;
 * far (background) shells shrink and dim into the night. Set FW_DEPTH 0 to put
 * every shell back on one plane. The whole trajectory scales uniformly, so a
 * burst reads as the same explosion seen from nearer or further, not a
 * differently-shaped one. */
#define FW_DEPTH         1         /* 1: vary distance; 0: all on one plane    */
#define FW_DEPTH_NEAR    1.45f     /* size scale of the nearest (foreground)   */
#define FW_DEPTH_FAR     0.45f     /* size scale of the farthest (background)  */
#define FW_DEPTH_BIAS    1.30f     /* >1 skews the field deeper (more far)     */
#define FW_DEPTH_DIM     0.42f     /* brightness of the farthest shells, 0..1  */
#define FW_NEAR_THICK    6         /* spark size of the nearest shells, px(even)*/
#define FW_DEPTH_SPARKS  0.30f     /* far shells shed up to this frac of sparks*/
#define FW_DEPTH_Y       0.12f     /* near bursts sit this much lower, far     *
                                    * higher (frac H; 0 = decouple from depth) */

/* --- ascent ------------------------------------------------------------ */
#define FW_LAUNCH_Y      1.04f     /* launch height, frac H (>1 = off bottom)  */
#define FW_LAUNCH_SPREAD 0.04f     /* launch x offset from burst column, frac W*/
#define FW_BURST_Y_MIN   0.16f     /* highest a shell bursts, frac H from top  */
#define FW_BURST_Y_MAX   0.55f     /* lowest a shell bursts                    */
#define FW_X_MARGIN      0.10f     /* keep bursts this far from side edges,frcW*/
#define FW_ASCENT_SECS   1.15f     /* nominal climb time to apex               */
#define FW_ASCENT_VAR    0.30f     /* +/- fraction on climb time               */
#define FW_RISE_TRAIL    7         /* rising rocket's sparkling tail (samples) */
#define FW_RISE_DT       0.018f    /* tail sample spacing, in climb-progress   */
#define FW_RISE_THK_HEAD 4         /* rocket head size px (even)               */
#define FW_RISE_THK_TAIL 2         /* rocket tail size px (even)               */
#define FW_RISE_WOBBLE   3.0f      /* px sway as the rocket climbs             */
#define FW_RISE_SHIMMER  22.0f     /* tail sparkle rate, Hz                    */

/* --- burst size & population ------------------------------------------- */
#define FW_SPARKS        58        /* base spark count per burst               */
#define FW_SPARKS_VAR    0.50f     /* +/- fraction on spark count              */
#define FW_SPARKS_MAX    120       /* hard cap per burst (perf)                */
#define FW_RADIUS        0.19f     /* base explosion radius, frac H            */
#define FW_RADIUS_VAR    0.06f     /* +/- frac H on radius (explosion-size var)*/
#define FW_SPEED_VAR     0.45f     /* per-spark speed spread (fills the sphere *
                                    * rather than a hollow rim)                */

/* --- burst dynamics ---------------------------------------------------- */
#define FW_BURST_SECS    1.9f      /* spark lifetime / burst duration, s       */
#define FW_BURST_VAR     0.30f     /* +/- fraction on lifetime                 */
#define FW_GRAVITY       0.34f     /* downward accel, frac H / s^2 (the droop) */
#define FW_DRAG          1.9f      /* air resistance, 1/s; higher = sparks     *
                                    * brake and hang sooner (radius unchanged) */
#define FW_ASPECT        1.0f      /* x-velocity scale; ~1.12 rounds bursts on *
                                    * a 720x480 buffer shown 4:3 on a CRT      */

/* --- spark appearance -------------------------------------------------- */
#define FW_SPARK_THICK   2         /* base spark size px (even)                */
#define FW_BACK_DIM      0.28f     /* brightness of the sphere's far side 0..1 */
#define FW_HOT_CORE      0.92f     /* birth flash toward white, 0..1           */
#define FW_HOT_FRAC      0.14f     /* fraction of life the white-hot core lasts*/
#define FW_FADE_POW      1.45f     /* fade curve; >1 lingers then drops away   */
#define FW_EMBER         0x551200u /* colour sparks cool toward near death     */
#define FW_EMBER_FROM    0.55f     /* life fraction where the ember tint begins*/
#define FW_EMBER_AMT     200       /* max blend toward ember, 0..256           */
#define FW_GLOW_AMT      95        /* phosphor halo around the head, 0..256(0=off)*/
#define FW_GLOW_PX       2         /* extra halo size px (even)                */

/* --- trails: the willow / chrysanthemum streak ------------------------- *
 * A spark's path is sampled at a few KNOTS, then joined into one continuous,
 * tapering line (see fw_line) so the trail is smooth, not spaced sprites. */
#define FW_TRAIL         4         /* trail knots (joined into one smooth line)*/
#define FW_TRAIL_MAX     12        /* cap on knots (long willow drips)         */
#define FW_TRAIL_DT      0.05f     /* seconds between knots                    */
#define FW_TRAIL_FADE    0.80f     /* how much the tail dims vs the head, 0..1 */
#define FW_TRAIL_TAPER   0.35f     /* tail thickness as a fraction of the head */

/* --- twinkle / strobe (the crackle at the end) ------------------------- */
#define FW_TWINKLE       0.70f     /* depth of end-of-life flicker (0 = off)   */
#define FW_TWINKLE_BASE  0.25f     /* baseline twinkle for ordinary shells     */
#define FW_TWINKLE_FROM  0.50f     /* life fraction where twinkle begins       */
#define FW_TWINKLE_HZ    9.0f      /* flicker rate, Hz                         */
#define FW_TWINKLE_LOW   0.25f     /* brightness of an "off" strobe beat       */

/* --- opening flash ----------------------------------------------------- */
#define FW_FLASH_SECS    0.16f     /* duration of the burst's white flash      */
#define FW_FLASH_PX      18        /* flash bloom size px                      */

/* --- burst-type mix (relative weights; edit to retune the show) -------- */
#define FW_W_PEONY    5            /* round filled sphere                      */
#define FW_W_RING     2            /* hollow ring (sparks on the equator)      */
#define FW_W_WILLOW   3            /* slow gold drips raining down             */
#define FW_W_CHRYS    4            /* sphere with long trailing tails          */
#define FW_W_PALM     2            /* a few thick fronds arching over          */
#define FW_W_GLITTER  2            /* silver sphere that crackles & strobes    */

/* --- per-type character ------------------------------------------------ */
#define FW_WILLOW_GRAV_MUL  1.9f
#define FW_WILLOW_DRAG_MUL  0.55f
#define FW_WILLOW_LIFE_MUL  1.7f   /* (also bounds the worst-case shell life)  */
#define FW_WILLOW_TRAIL_MUL 2.6f
#define FW_WILLOW_GOLD      1      /* willow cools toward gold                 */
#define FW_PALM_SPARK_MUL   0.45f  /* fewer, fatter fronds                     */
#define FW_PALM_UPBIAS      0.50f  /* upward velocity bias -> rises then arcs  */
#define FW_GLITTER_TWINKLE  1.6f
#define FW_GLITTER_SILVER   1

/* --- fake scanlines ---------------------------------------------------- *
 * The CRT lays down real scanlines, so default OFF. On a progressive host
 * preview a small value (~24) adds the retro look. 0..256 dimming of odd lines.
 */
#define FW_SCANLINE_AMT  0

/* Traditional firework palette, used when FW_THEME_COLORS == 0. */
static const uint32_t fw_trad[] = {
    0xFF2A2Au, /* red       */ 0xFF7A18u, /* orange   */
    0xFFC400u, /* gold      */ 0xFFF1A8u, /* warm white*/
    0x35FF45u, /* green     */ 0x18C8FFu, /* cyan     */
    0x3A6BFFu, /* blue      */ 0xC04BFFu, /* violet   */
    0xFF52C4u, /* pink      */ 0xEAEFFFu, /* silver   */
};

enum { FW_PEONY, FW_RING, FW_WILLOW, FW_CHRYS, FW_PALM, FW_GLITTER, FW_N_TYPES };
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

/* ------------------------------ Fireworks ------------------------------ */

/* Deterministic per-shell / per-spark randoms. hash_u32 already avalanches,
 * so feeding it a salted mix of the indices gives independent 0..1 streams. */
static inline float fw_h1(uint32_t shell, uint32_t salt)
{
    return hash01(shell * 0x9E3779B9u + salt * 0x85EBCA6Bu + 0x165667B1u);
}
static inline float fw_h2(uint32_t shell, uint32_t spark, uint32_t salt)
{
    return hash01((shell * 0x9E3779B9u) ^ (spark * 0x85EBCA6Bu)
                  ^ (salt * 0xC2B2AE35u));
}
static inline float fw_sym(uint32_t shell, uint32_t salt)  /* -1..1 */
{
    return fw_h1(shell, salt) * 2.0f - 1.0f;
}

/* A colour drawn from the active source (theme tones or the trad palette). */
static uint32_t fw_pick(const ui_theme_t *th, float r)
{
    if (FW_THEME_COLORS) {
        const uint32_t pal[4] = { th->bright, th->hot, th->bar, th->text };
        if (FW_THEME_BLEND) {        /* slide across pal[0..3] for variety */
            float f = r * 2.999f;
            int i = (int)f; if (i > 2) i = 2;
            return ui_lerp(pal[i], pal[i + 1], (int)((f - (float)i) * 256.0f));
        }
        int i = (int)(r * 4.0f); if (i > 3) i = 3;
        return pal[i];
    }
    const int n = (int)(sizeof fw_trad / sizeof fw_trad[0]);
    int i = (int)(r * (float)n); if (i >= n) i = n - 1;
    return fw_trad[i];
}

/* Weighted pick of a burst type from the FW_W_* mix. */
static int fw_type(uint32_t shell)
{
    static const int w[FW_N_TYPES] = { FW_W_PEONY, FW_W_RING, FW_W_WILLOW,
                                       FW_W_CHRYS, FW_W_PALM, FW_W_GLITTER };
    int tot = 0;
    for (int i = 0; i < FW_N_TYPES; i++) tot += w[i];
    int r = (int)(fw_h1(shell, 31u) * (float)tot), acc = 0;
    for (int i = 0; i < FW_N_TYPES; i++) {
        acc += w[i];
        if (r < acc) return i;
    }
    return FW_PEONY;
}

typedef struct {
    float lx, ly, bx, by;   /* launch and burst points, px */
    float ascent;           /* climb time to apex, s */
    float wob;              /* climb sway phase */
    int   type, nsparks, thick;
    float radius, drag, grav, life, upbias;
    float trailmul, twinkmul;
    float scale, dim;       /* depth: size scale and brightness (1 = near plane)*/
    int   multicolor;
    uint32_t color;         /* shell base colour (mono shells, rocket, flash) */
} fw_shell_t;

static fw_shell_t fw_make_shell(const ui_theme_t *th, uint32_t k,
                                const ui_canvas_t *c)
{
    float W = (float)c->w, H = (float)c->h;
    fw_shell_t s;
    s.type    = fw_type(k);
    /* depth: dn 0 (far) .. 1 (near); BIAS>1 deepens the field. Near shells are
     * larger, brighter, thicker and (in render) drawn last, over the far ones. */
    float dn  = powf(fw_h1(k, 41u), FW_DEPTH_BIAS);
    s.scale   = FW_DEPTH ? FW_DEPTH_FAR + (FW_DEPTH_NEAR - FW_DEPTH_FAR) * dn
                         : 1.0f;
    s.dim     = FW_DEPTH ? FW_DEPTH_DIM + (1.0f - FW_DEPTH_DIM) * dn : 1.0f;
    int dthick = FW_DEPTH
        ? (int)(FW_SPARK_THICK + (FW_NEAR_THICK - FW_SPARK_THICK) * dn + 0.5f)
        : FW_SPARK_THICK;
    s.bx      = (FW_X_MARGIN + fw_h1(k, 11u) * (1.0f - 2.0f * FW_X_MARGIN)) * W;
    s.by      = (FW_BURST_Y_MIN
               + fw_h1(k, 12u) * (FW_BURST_Y_MAX - FW_BURST_Y_MIN)) * H
               + (FW_DEPTH ? (dn - 0.5f) * FW_DEPTH_Y * H : 0.0f);
    s.lx      = s.bx + fw_sym(k, 13u) * FW_LAUNCH_SPREAD * W;
    s.ly      = FW_LAUNCH_Y * H;
    s.ascent  = FW_ASCENT_SECS * (1.0f + fw_sym(k, 14u) * FW_ASCENT_VAR);
    s.wob     = fw_h1(k, 21u) * TAU;
    s.radius  = (FW_RADIUS + fw_sym(k, 15u) * FW_RADIUS_VAR) * H * s.scale;
    s.nsparks = (int)(FW_SPARKS * (1.0f + fw_sym(k, 16u) * FW_SPARKS_VAR)
                      * (FW_DEPTH ? 1.0f - FW_DEPTH_SPARKS * (1.0f - dn) : 1.0f));
    s.life    = FW_BURST_SECS * (1.0f + fw_sym(k, 17u) * FW_BURST_VAR);
    s.drag    = FW_DRAG;
    s.grav    = FW_GRAVITY * H * s.scale;
    s.upbias  = 0.0f;
    s.trailmul = 1.0f;
    s.twinkmul = FW_TWINKLE_BASE;
    s.thick   = dthick;
    s.multicolor = FW_MULTICOLOR && (fw_h1(k, 18u) < FW_MULTICOLOR_CHANCE);
    s.color   = fw_pick(th, fw_h1(k, 19u));

    switch (s.type) {
    case FW_RING:    s.nsparks = (int)(s.nsparks * 1.10f); s.trailmul = 0.6f;
                     break;
    case FW_WILLOW:  s.grav *= FW_WILLOW_GRAV_MUL; s.drag *= FW_WILLOW_DRAG_MUL;
                     s.life *= FW_WILLOW_LIFE_MUL; s.trailmul = FW_WILLOW_TRAIL_MUL;
                     s.radius *= 0.85f;
                     if (FW_WILLOW_GOLD)
                         s.color = FW_THEME_COLORS ? th->hot : 0xFFB030u;
                     break;
    case FW_CHRYS:   s.trailmul = 1.8f; break;
    case FW_PALM:    s.nsparks = (int)(s.nsparks * FW_PALM_SPARK_MUL);
                     s.thick += 2; s.trailmul = 2.2f;
                     s.upbias = FW_PALM_UPBIAS; s.grav *= 1.4f; break;
    case FW_GLITTER: s.twinkmul = FW_GLITTER_TWINKLE; s.trailmul = 0.5f;
                     if (FW_GLITTER_SILVER)
                         s.color = FW_THEME_COLORS ? th->bright : 0xEAEFFFu;
                     break;
    default: break;   /* PEONY */
    }
    if (s.nsparks < 8) s.nsparks = 8;
    if (s.nsparks > FW_SPARKS_MAX) s.nsparks = FW_SPARKS_MAX;
    s.thick -= (s.thick & 1); if (s.thick < 2) s.thick = 2;
    if (s.drag < 0.01f) s.drag = 0.01f;   /* it divides the trajectory */
    return s;
}

/* a head spark, with the optional phosphor halo behind it */
static inline void fw_spark(ui_canvas_t *c, float x, float y, int thick,
                            uint32_t col)
{
    if (FW_GLOW_AMT > 0)
        plot(c, x, y, thick + FW_GLOW_PX, ui_lerp(FW_BG, col, FW_GLOW_AMT));
    plot(c, x, y, thick, col);
}

/* A gap-free, tapering segment between two trail knots: lay blocks ~every
 * 2 px (the warp-comet trick) with thickness and colour lerped end to end, so
 * a few curved knots read as one smooth streak rather than spaced sprites. */
static void fw_line(ui_canvas_t *c, float x0, float y0, int t0, uint32_t c0,
                    float x1, float y1, int t1, uint32_t c1)
{
    float dx = x1 - x0, dy = y1 - y0;
    int steps = (int)(sqrtf(dx * dx + dy * dy) * 0.5f);
    if (steps < 1) steps = 1; else if (steps > 64) steps = 64;
    for (int i = 0; i <= steps; i++) {
        float f = (float)i / (float)steps;
        int thk = (int)(t0 + (t1 - t0) * f + 0.5f);
        thk -= thk & 1; if (thk < 2) thk = 2;
        plot(c, x0 + dx * f, y0 + dy * f, thk, ui_lerp(c0, c1, (int)(f * 256.0f)));
    }
}

static void fw_rocket(ui_canvas_t *c, const fw_shell_t *s, float age)
{
    float u = age / s->ascent;
    if (u < 0.0f) u = 0.0f; else if (u > 1.0f) u = 1.0f;
    uint32_t warm = ui_lerp(s->color, 0xFFFFFFu, 120);
    int hthk = (int)(FW_RISE_THK_HEAD * s->scale + 0.5f); hthk -= hthk & 1;
    int tthk = (int)(FW_RISE_THK_TAIL * s->scale + 0.5f); tthk -= tthk & 1;
    if (hthk < 2) hthk = 2;
    if (tthk < 2) tthk = 2;
    for (int i = FW_RISE_TRAIL; i >= 0; i--) {
        float uu = u - (float)i * FW_RISE_DT;
        if (uu < 0.0f) continue;
        float ey = 1.0f - (1.0f - uu) * (1.0f - uu);   /* decelerate to apex */
        float x  = s->lx + (s->bx - s->lx) * uu
                 + FW_RISE_WOBBLE * sinf(uu * 5.0f + s->wob);
        float y  = s->ly + (s->by - s->ly) * ey;
        float sh = 0.6f + 0.4f * sinf(TAU * (FW_RISE_SHIMMER * age + (float)i * 0.7f));
        float b  = (i == 0) ? 1.0f
                            : (1.0f - 0.9f * (float)i / (float)FW_RISE_TRAIL) * sh;
        int t = (int)(clamp01(b) * s->dim * 256.0f);
        if (i == 0)
            fw_spark(c, x, y, hthk,
                     ui_lerp(FW_BG, ui_lerp(warm, 0xFFFFFFu, 90), t));
        else
            plot(c, x, y, tthk, ui_lerp(FW_BG, warm, t));
    }
}

static void fw_burst(ui_canvas_t *c, const ui_theme_t *th, const fw_shell_t *s,
                     uint32_t k, float ba, float t)
{
    if (ba < FW_FLASH_SECS) {                 /* the opening flash */
        float ff = 1.0f - ba / FW_FLASH_SECS;
        uint32_t white = ui_lerp(s->color, 0xFFFFFFu, 210);
        int big = (int)(FW_FLASH_PX * s->scale * (0.5f + 0.8f * ff));
        big -= (big & 1);
        int sm  = big / 2; sm -= (sm & 1);
        if (big < 2) big = 2;
        if (sm  < 2) sm  = 2;
        plot(c, s->bx, s->by, big, ui_lerp(FW_BG, white, (int)(ff*90.0f*s->dim)));
        plot(c, s->bx, s->by, sm,  ui_lerp(FW_BG, white, (int)(ff*220.0f*s->dim)));
    }

    float f = ba / s->life;                   /* life fraction 0..1 */
    float env = powf(clamp01(1.0f - f), FW_FADE_POW);
    if (env <= 0.003f) return;

    for (int j = 0; j < s->nsparks; j++) {
        /* initial velocity: a uniform point on the unit sphere; z shades depth.
         * RING flattens to the equator. SPEED_VAR fills the interior. */
        float zc = (s->type == FW_RING) ? 0.0f
                                        : 1.0f - 2.0f * fw_h2(k, (uint32_t)j, 1u);
        float rr = sqrtf(1.0f - zc * zc);
        float ph = TAU * fw_h2(k, (uint32_t)j, 2u);
        float spd = s->radius * s->drag
                  * (1.0f - FW_SPEED_VAR * fw_h2(k, (uint32_t)j, 3u));
        float vx = rr * cosf(ph) * spd * FW_ASPECT;
        float vy = rr * sinf(ph) * spd - s->upbias * spd;
        float depth = FW_BACK_DIM + (1.0f - FW_BACK_DIM) * (0.5f + 0.5f * zc);
        int thick = s->thick + (zc > 0.45f ? 2 : 0);

        float tw = 1.0f;                       /* end-of-life strobe */
        if (s->twinkmul > 0.0f && f > FW_TWINKLE_FROM) {
            float fl = sinf(TAU * (FW_TWINKLE_HZ * t + fw_h2(k, (uint32_t)j, 5u)))
                       > 0.0f ? 1.0f : FW_TWINKLE_LOW;
            float amt = FW_TWINKLE * s->twinkmul
                      * clamp01((f - FW_TWINKLE_FROM) / (1.0f - FW_TWINKLE_FROM));
            if (amt > 1.0f) amt = 1.0f;
            tw = 1.0f - amt * (1.0f - fl);
        }

        uint32_t base = s->multicolor ? fw_pick(th, fw_h2(k, (uint32_t)j, 9u))
                                       : s->color;
        float core = FW_HOT_CORE * clamp01(1.0f - f / FW_HOT_FRAC);
        uint32_t col = ui_lerp(base, 0xFFFFFFu, (int)(core * 256.0f));
        if (f > FW_EMBER_FROM) {
            float em = clamp01((f - FW_EMBER_FROM) / (1.0f - FW_EMBER_FROM));
            col = ui_lerp(col, FW_EMBER, (int)(em * FW_EMBER_AMT));
        }
        float bhead = env * depth * tw * s->dim;

        /* Draw the spark as one smooth, tapering streak: walk the curved path
         * from tail to head joining connected, gap-free blocks instead of a
         * handful of spaced sprites. Only the knots cost an expf. */
        int nt = (int)(FW_TRAIL * s->trailmul + 0.5f);
        if (nt < 1) nt = 1; else if (nt > FW_TRAIL_MAX) nt = FW_TRAIL_MAX;
        float vss = s->grav / s->drag;          /* terminal fall speed */
        float px = 0.0f, py = 0.0f; int pthk = 0; uint32_t pcc = 0; int have = 0;
        for (int ts = nt - 1; ts >= 0; ts--) {
            float bat = ba - (float)ts * FW_TRAIL_DT;
            if (bat < 0.0f) { have = 0; continue; }   /* knot not born yet */
            float disp = (1.0f - expf(-s->drag * bat)) / s->drag;
            float x = s->bx + vx * disp;
            float y = s->by + vss * bat + (vy - vss) * disp;
            float kf = (nt > 1) ? 1.0f - (float)ts / (float)(nt - 1) : 1.0f;
            int b = (int)(bhead * (1.0f - FW_TRAIL_FADE * (1.0f - kf)) * 256.0f);
            if (b < 0) b = 0; else if (b > 256) b = 256;
            uint32_t cc = ui_lerp(FW_BG, col, b);
            int kthk = (int)(thick * (FW_TRAIL_TAPER
                                      + (1.0f - FW_TRAIL_TAPER) * kf) + 0.5f);
            kthk -= kthk & 1; if (kthk < 2) kthk = 2;
            if (have) fw_line(c, px, py, pthk, pcc, x, y, kthk, cc);
            px = x; py = y; pthk = kthk; pcc = cc; have = 1;
        }
        if (have) fw_spark(c, px, py, pthk, pcc);    /* glowing head */
    }
}

#if FW_SCANLINE_AMT > 0
static void fw_scanlines(ui_canvas_t *c)
{
    for (int y = 1; y < c->h; y += 2) {
        uint32_t *row = (uint32_t *)(c->px + (size_t)y * c->stride);
        for (int x = 0; x < c->w; x++)
            row[x] = ui_lerp(row[x], 0x000000u, FW_SCANLINE_AMT);
    }
}
#endif

static void render_fireworks(ui_canvas_t *c, const ui_theme_t *th,
                             unsigned tick)
{
    uint32_t sky = FW_BG;
    if (FW_BG_THEME)
        sky = ui_lerp(0x000000u, th->bg, FW_BG_THEME_AMT);
    ui_fill(c, 0, 0, c->w, c->h, sky);

    float t = (float)tick / HZ;             /* seconds */
    float interval = 1.0f / FW_RATE;
    /* furthest-back shell that could still be alive (worst-case willow life) */
    float maxlife = FW_ASCENT_SECS * (1.0f + FW_ASCENT_VAR)
                  + FW_BURST_SECS * (1.0f + FW_BURST_VAR) * FW_WILLOW_LIFE_MUL
                  + interval;
    int k1 = (int)floorf(t / interval);
    int k0 = (int)floorf((t - maxlife) / interval);

    /* Gather the live shells (newest first, capped), then paint them far->near
     * so a foreground burst layers over the distant ones — the only place the
     * stateless field needs ordering. */
    struct fw_active { fw_shell_t s; float age; uint32_t k; };
    struct fw_active act[FW_MAX_SHELLS];
    int nact = 0;
    for (int k = k1; k >= k0 && nact < FW_MAX_SHELLS; k--) {
        fw_shell_t s = fw_make_shell(th, (uint32_t)k, c);
        float tb  = (float)k * interval + fw_sym((uint32_t)k, 99u)
                                          * interval * FW_RATE_VAR;
        float age = t - tb;
        if (age < 0.0f) continue;                 /* not launched yet */
        if (age >= s.ascent + s.life) continue;   /* burnt out */
        act[nact].s = s; act[nact].age = age; act[nact].k = (uint32_t)k;
        nact++;
    }
    /* insertion sort by scale ascending: far (small) drawn first */
    for (int i = 1; i < nact; i++)
        for (int j = i; j > 0 && act[j].s.scale < act[j - 1].s.scale; j--) {
            struct fw_active tmp = act[j];
            act[j] = act[j - 1];
            act[j - 1] = tmp;
        }
    for (int i = 0; i < nact; i++) {
        const fw_shell_t *s = &act[i].s;
        float age = act[i].age;
        if (age < s->ascent) fw_rocket(c, s, age);
        else                 fw_burst(c, th, s, act[i].k, age - s->ascent, t);
    }

#if FW_SCANLINE_AMT > 0
    fw_scanlines(c);
#endif
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
    case PIDVD_SAVER_FIREWORKS:
        render_fireworks(c, th, tick);
        break;
    case PIDVD_SAVER_OFF:
    default:
        break;   /* canvas is already the theme background */
    }
}
