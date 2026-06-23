#include "decode/audio_a52.h"

#include <stdlib.h>
#include <string.h>

#include <a52dec/a52.h>

#define ABUF 8192
#define PTS_MARKERS 16

struct pts_marker {
    size_t offset;
    int64_t pts;
};

struct pidvd_adec {
    a52_state_t *state;
    pidvd_audio_cb cb;
    void *ctx;
    uint8_t buf[ABUF];
    size_t have;
    int64_t next_pts;
    int64_t pts_remainder;
    struct pts_marker marker[PTS_MARKERS];
    int marker_count;
    int16_t out[1536 * 2];
};

pidvd_adec_t *pidvd_adec_new(pidvd_audio_cb cb, void *ctx)
{
    pidvd_adec_t *a = calloc(1, sizeof(*a));
    a->state = a52_init(0);
    a->cb = cb;
    a->ctx = ctx;
    a->next_pts = -1;
    return a;
}

void pidvd_adec_reset(pidvd_adec_t *a)
{
    a->have = 0;
    a->next_pts = -1;
    a->pts_remainder = 0;
    a->marker_count = 0;
}

static void emit(pidvd_adec_t *a, const uint8_t *frame, int srate,
                 int64_t pts)
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
    a->cb(a->ctx, a->out, 1536, srate, pts);
    if (pts >= 0) {
        int64_t ticks = INT64_C(1536) * 90000 + a->pts_remainder;
        a->next_pts = pts + ticks / srate;
        a->pts_remainder = ticks % srate;
    }
}

static void add_marker(pidvd_adec_t *a, size_t offset, int64_t pts)
{
    if (a->marker_count == PTS_MARKERS) {
        memmove(a->marker, a->marker + 1,
                (PTS_MARKERS - 1) * sizeof(a->marker[0]));
        a->marker_count--;
    }
    a->marker[a->marker_count++] = (struct pts_marker) {
        .offset = offset,
        .pts = pts,
    };
}

static int64_t pts_at_frame(pidvd_adec_t *a, size_t frame_offset)
{
    int64_t pts = -1;
    int used = 0;
    while (used < a->marker_count
           && a->marker[used].offset <= frame_offset) {
        pts = a->marker[used].pts;
        used++;
    }
    if (used) {
        memmove(a->marker, a->marker + used,
                (size_t)(a->marker_count - used) * sizeof(a->marker[0]));
        a->marker_count -= used;
        a->pts_remainder = 0;
        return pts;
    }
    return a->next_pts;
}

void pidvd_adec_push(pidvd_adec_t *a, const uint8_t *data, size_t len,
                     int64_t pts)
{
    if (pts >= 0)
        add_marker(a, a->have, pts);
    if (a->have + len > ABUF) {
        a->have = 0;   /* overflow: resync */
        a->marker_count = 0;
        a->next_pts = -1;
    }
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
        int64_t frame_pts = pts_at_frame(a, pos);
        emit(a, a->buf + pos, srate, frame_pts);
        pos += flen;
    }
    memmove(a->buf, a->buf + pos, a->have - pos);
    a->have -= pos;
    for (int i = 0; i < a->marker_count; i++)
        a->marker[i].offset =
            a->marker[i].offset > pos ? a->marker[i].offset - pos : 0;
}

void pidvd_adec_free(pidvd_adec_t *a)
{
    if (!a)
        return;
    a52_free(a->state);
    free(a);
}
