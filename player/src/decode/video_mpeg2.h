/* libmpeg2 wrapper: push video ES bytes, get RGB32 frames + field flags. */
#ifndef PIDVD_DECODE_VIDEO_MPEG2_H
#define PIDVD_DECODE_VIDEO_MPEG2_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct pidvd_vdec pidvd_vdec_t;

/* planar YCbCr 4:2:0; chroma is width/2 x height/2 */
typedef void (*pidvd_frame_cb)(void *ctx, const uint8_t *y,
                               const uint8_t *u, const uint8_t *v,
                               int width, int height,
                               bool tff, bool rff, int64_t pts);

pidvd_vdec_t *pidvd_vdec_new(pidvd_frame_cb cb, void *ctx);
void pidvd_vdec_push(pidvd_vdec_t *d, const uint8_t *data,
                     size_t len, int64_t pts);
void pidvd_vdec_reset(pidvd_vdec_t *d);  /* flush on seek/branch */
void pidvd_vdec_free(pidvd_vdec_t *d);

#endif
