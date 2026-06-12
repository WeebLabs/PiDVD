#include "decode/video_mpeg2.h"

#include <stdlib.h>
#include <string.h>

#include <mpeg2dec/mpeg2.h>

struct pidvd_vdec {
    mpeg2dec_t *dec;
    const mpeg2_info_t *info;
    pidvd_frame_cb cb;
    void *ctx;
    /* libmpeg2 keeps references into the buffer we hand it, so feed it
     * from our own stable copy */
    uint8_t chunk[4096];
};

pidvd_vdec_t *pidvd_vdec_new(pidvd_frame_cb cb, void *ctx)
{
    pidvd_vdec_t *d = calloc(1, sizeof(*d));
    d->dec = mpeg2_init();
    if (!d->dec) {
        free(d);
        return NULL;
    }
    d->info = mpeg2_info(d->dec);
    d->cb = cb;
    d->ctx = ctx;
    return d;
}

static void drain(pidvd_vdec_t *d)
{
    mpeg2_state_t state;
    while ((state = mpeg2_parse(d->dec)) != STATE_BUFFER) {
        switch (state) {
        case STATE_SEQUENCE:
            break;   /* planar output; conversion runs on the
                      * presenter core */
        case STATE_SLICE:
        case STATE_END:
        case STATE_INVALID_END:
            if (d->info->display_fbuf && d->info->sequence) {
                const mpeg2_picture_t *pic = d->info->display_picture;
                bool tff = pic && (pic->flags & PIC_FLAG_TOP_FIELD_FIRST);
                bool rff = pic && pic->nb_fields > 2;
                d->cb(d->ctx, d->info->display_fbuf->buf[0],
                      d->info->display_fbuf->buf[1],
                      d->info->display_fbuf->buf[2],
                      (int)d->info->sequence->width,
                      (int)d->info->sequence->height, tff, rff);
            }
            break;
        default:
            break;
        }
    }
}

void pidvd_vdec_push(pidvd_vdec_t *d, const uint8_t *data, size_t len)
{
    while (len > 0) {
        size_t n = len < sizeof(d->chunk) ? len : sizeof(d->chunk);
        memcpy(d->chunk, data, n);
        mpeg2_buffer(d->dec, d->chunk, d->chunk + n);
        drain(d);
        data += n;
        len -= n;
    }
}

void pidvd_vdec_reset(pidvd_vdec_t *d)
{
    mpeg2_reset(d->dec, 1);
}

void pidvd_vdec_free(pidvd_vdec_t *d)
{
    if (!d)
        return;
    mpeg2_close(d->dec);
    free(d);
}
