#include "decode/audio_a52.h"

#include <stdlib.h>
#include <string.h>

#include <a52dec/a52.h>

#define ABUF 8192

struct pidvd_adec {
    a52_state_t *state;
    pidvd_audio_cb cb;
    void *ctx;
    uint8_t buf[ABUF];
    size_t have;
    int64_t pts;           /* pts of the next frame to start in buf */
    int64_t pending_pts;   /* pts attached to newly pushed bytes */
    int16_t out[1536 * 2];
};

pidvd_adec_t *pidvd_adec_new(pidvd_audio_cb cb, void *ctx)
{
    pidvd_adec_t *a = calloc(1, sizeof(*a));
    a->state = a52_init(0);
    a->cb = cb;
    a->ctx = ctx;
    a->pts = a->pending_pts = -1;
    return a;
}

void pidvd_adec_reset(pidvd_adec_t *a)
{
    a->have = 0;
    a->pts = a->pending_pts = -1;
}

static void emit(pidvd_adec_t *a, const uint8_t *frame, int srate)
{
    int flags = A52_STEREO | A52_ADJUST_LEVEL;
    sample_t level = 1.0, bias = 0.0;
    if (a52_frame(a->state, (uint8_t *)frame, &flags, &level, bias))
        return;
    sample_t *s = a52_samples(a->state);
    for (int blk = 0; blk < 6; blk++) {
        if (a52_block(a->state))
            return;
        int16_t *o = a->out + blk * 256 * 2;
        for (int i = 0; i < 256; i++) {
            float l = s[i] * 32767.0f;
            float r = s[i + 256] * 32767.0f;
            o[i * 2] = l > 32767 ? 32767 : (l < -32768 ? -32768 : (int16_t)l);
            o[i * 2 + 1] = r > 32767 ? 32767 : (r < -32768 ? -32768
                                                           : (int16_t)r);
        }
    }
    a->cb(a->ctx, a->out, 1536, srate, a->pts);
    a->pts = -1;   /* consumed; next frame inherits pending */
}

void pidvd_adec_push(pidvd_adec_t *a, const uint8_t *data, size_t len,
                     int64_t pts)
{
    if (pts >= 0)
        a->pending_pts = pts;
    if (a->have + len > ABUF)
        a->have = 0;   /* overflow: resync */
    memcpy(a->buf + a->have, data, len);
    a->have += len;

    size_t pos = 0;
    for (;;) {
        /* find sync */
        while (pos + 7 <= a->have
               && !(a->buf[pos] == 0x0b && a->buf[pos + 1] == 0x77))
            pos++;
        if (pos + 7 > a->have)
            break;
        int flags, srate, brate;
        int flen = a52_syncinfo(a->buf + pos, &flags, &srate, &brate);
        if (flen <= 0) {
            pos++;
            continue;
        }
        if (pos + (size_t)flen > a->have)
            break;
        if (a->pts < 0)
            a->pts = a->pending_pts, a->pending_pts = -1;
        emit(a, a->buf + pos, srate);
        pos += flen;
    }
    memmove(a->buf, a->buf + pos, a->have - pos);
    a->have -= pos;
}

void pidvd_adec_free(pidvd_adec_t *a)
{
    if (!a)
        return;
    a52_free(a->state);
    free(a);
}
