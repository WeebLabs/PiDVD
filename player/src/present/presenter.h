/* Presenter: decouples decode from display. The decoder pushes finished
 * frames into a small ring (blocking when full — natural backpressure);
 * a dedicated thread pops one frame per vsync-paced flip, so decode-time
 * variance never reaches the screen. */
#ifndef PIDVD_PRESENT_PRESENTER_H
#define PIDVD_PRESENT_PRESENTER_H

#include <stdbool.h>
#include <stdint.h>

#include "platform/platform.h"

typedef struct pidvd_presenter pidvd_presenter_t;

pidvd_presenter_t *pidvd_presenter_start(pidvd_video_t *video,
                                         int width, int height);
/* Blocks while the ring is full. */
void pidvd_presenter_push(pidvd_presenter_t *p, const uint8_t *rgb32,
                          int width, int height, int stride,
                          bool tff, bool rff);
/* Drain remaining frames, stop the thread; returns frames shown. */
long pidvd_presenter_stop(pidvd_presenter_t *p);

#endif
