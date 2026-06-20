/* Screensavers. One selectable effect today — a warp starfield — with the
 * enum and render entry shaped so more can drop in later. Pure pixel code
 * over ui_canvas_t, like the rest of the picker UI: a view-model frame in,
 * pixels out, animation driven solely by the field-rate tick. No state is
 * carried between frames, so it composes with picker.c's stateless render
 * path. See ui/saver.c for the tunables. docs/UI.md §5.7. */
#ifndef PIDVD_UI_SAVER_H
#define PIDVD_UI_SAVER_H

#include "ui/draw.h"     /* ui_canvas_t */
#include "ui/render.h"   /* ui_theme_t  */

/* Screensaver kinds. The values are the SCREENSAVER setting's indices
 * (settings.c saver_v[]), so these must match OFF / WARP STARFIELD / DVD
 * LOGO there. */
enum { PIDVD_SAVER_OFF = 0, PIDVD_SAVER_WARP = 1, PIDVD_SAVER_DVD = 2 };

/* How long the picker must see no input before the screensaver arms, in
 * real seconds. Deliberately a code-side knob (you asked for the timing to
 * be tunable in code) and distinct from the SETTINGS "ATTRACT DIM" option.
 * The picker loop ticks at the menu field rate, so seconds -> frames needs
 * that rate; the 240p NTSC menu runs ~60 fields/s. */
#define PIDVD_SAVER_FIELD_HZ      60
#define PIDVD_SAVER_IDLE_SECONDS  90
#define PIDVD_SAVER_IDLE_FRAMES \
    ((unsigned)(PIDVD_SAVER_IDLE_SECONDS) * (unsigned)(PIDVD_SAVER_FIELD_HZ))

/* Draw one animation frame of screensaver `kind` onto c, which the caller
 * has already cleared to the theme background. `tick` is the field-rate
 * frame counter (ui_view_t.tick). Colours are pulled from `th`, so the
 * effect tracks whatever theme is active. PIDVD_SAVER_OFF draws nothing. */
void pidvd_saver_render(ui_canvas_t *c, const ui_theme_t *th,
                        int kind, unsigned tick);

#endif
