/* Disc-loading animation, played in the picker after an ISO is chosen and
 * before playback starts. Stateless like the screensaver: one frame computed
 * from the field-rate tick t (0..PIDVD_LOAD_ANIM_FRAMES). (sx,sy) is where the
 * chosen disc icon sat in the list, so the disc can fly out of it. ~3 s. */
#ifndef PIDVD_UI_LOAD_ANIM_H
#define PIDVD_UI_LOAD_ANIM_H

#include "ui/draw.h"
#include "ui/render.h"

#define PIDVD_LOAD_ANIM_FRAMES 180   /* ~3 s at the ~60 field/s menu rate */

void pidvd_load_anim_render(ui_canvas_t *c, const ui_theme_t *th,
                            int sx, int sy, unsigned t);

#endif
