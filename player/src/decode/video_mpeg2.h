/* libmpeg2 wrapper: push video ES bytes, get RGB32 frames + field flags. */
#ifndef PIDVD_DECODE_VIDEO_MPEG2_H
#define PIDVD_DECODE_VIDEO_MPEG2_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct pidvd_vdec pidvd_vdec_t;

typedef void (*pidvd_frame_cb)(void *ctx, const uint8_t *rgb32,
                               int width, int height, int stride,
                               bool tff, bool rff);

pidvd_vdec_t *pidvd_vdec_new(pidvd_frame_cb cb, void *ctx);
void pidvd_vdec_push(pidvd_vdec_t *d, const uint8_t *data, size_t len);
void pidvd_vdec_free(pidvd_vdec_t *d);

#endif
