/* Presenter: decode/display decoupling plus the display-side pipeline.
 * The decoder pushes planar YUV 4:2:0 frames into a ring (blocking when
 * full); the presenter thread converts to RGB on its own core, lets the
 * caller blend overlays via the hook, and pushes to the video output at
 * vsync pace. */
#ifndef PIDVD_PRESENT_PRESENTER_H
#define PIDVD_PRESENT_PRESENTER_H

#include <stdbool.h>
#include <stdint.h>

#include "platform/platform.h"

typedef struct pidvd_presenter pidvd_presenter_t;

/* Called on the presenter thread after YUV->RGB conversion, before
 * scanout; blend overlays into rgb (w*h*4, XRGB little-endian). */
typedef void (*pidvd_blend_cb)(void *ctx, uint8_t *rgb, int w, int h);
/* Called before the first frame of each unrelated PTS epoch. It may briefly
 * wait for audio prebuffering; it must never make cadence decisions. */
typedef void (*pidvd_prepare_cb)(void *ctx, uint64_t epoch);
/* Successful physical presentation, used to publish the display clock. */
typedef void (*pidvd_presented_cb)(void *ctx, uint64_t epoch, int64_t pts,
                                   const pidvd_video_stamp_t *stamp);

pidvd_presenter_t *pidvd_presenter_start(pidvd_video_t *video,
                                         int width, int height,
                                         pidvd_blend_cb blend, void *ctx,
                                         pidvd_prepare_cb prepare,
                                         pidvd_presented_cb presented,
                                         void *timing_ctx);
/* Blocks while the ring is full. Planar 4:2:0. */
void pidvd_presenter_push(pidvd_presenter_t *p,
                          const uint8_t *y, const uint8_t *u,
                          const uint8_t *v, int width, int height,
                          bool tff, bool rff, int64_t pts, uint64_t epoch);
/* Discard queued frames from an obsolete epoch. Waits only for an in-flight
 * flip/conversion to finish, so ring storage can never be overwritten live. */
void pidvd_presenter_reset(pidvd_presenter_t *p);
/* Hold the displayed frame (pause) or release it (resume). While held the
 * presenter stops advancing and stops publishing the display clock. */
void pidvd_presenter_set_paused(pidvd_presenter_t *p, bool paused);
/* Drain remaining frames, stop the thread; returns frames shown. */
long pidvd_presenter_stop(pidvd_presenter_t *p);

#endif
