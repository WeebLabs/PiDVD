#include "demux/ps.h"

#include <string.h>

void pidvd_ps_init(pidvd_ps_t *ps, pidvd_es_cb video_cb, void *ctx)
{
    ps->video_cb = video_cb;
    ps->spu_cb = 0;
    ps->audio_cb = 0;
    ps->ctx = ctx;
}

static int64_t pes_pts(const uint8_t *body, size_t len)
{
    if (len < 8 || !(body[1] & 0x80))
        return -1;
    const uint8_t *p = body + 3;
    return ((int64_t)((p[0] >> 1) & 7) << 30) | ((int64_t)p[1] << 22)
         | ((int64_t)(p[2] >> 1) << 15) | ((int64_t)p[3] << 7)
         | (p[4] >> 1);
}

/* DVD VOB sectors are self-contained: a pack header at offset 0, then
 * PES packets to the end of the 2048-byte sector. */
void pidvd_ps_push_sector(pidvd_ps_t *ps, const uint8_t s[2048])
{
    size_t off = 0;

    /* pack header: 00 00 01 BA, 10 bytes of SCR/mux info, stuffing len */
    if (!(s[0] == 0 && s[1] == 0 && s[2] == 1 && s[3] == 0xBA))
        return;
    off = 14 + (s[13] & 7);

    while (off + 6 <= 2048) {
        if (!(s[off] == 0 && s[off + 1] == 0 && s[off + 2] == 1))
            break;
        uint8_t id = s[off + 3];
        size_t len = ((size_t)s[off + 4] << 8) | s[off + 5];
        size_t body = off + 6;
        if (body + len > 2048)
            break;

        if (id == 0xBB || id == 0xBE || id == 0xBF) {
            /* system header / padding / nav — skip */
        } else if (id >= 0xE0 && id <= 0xEF) {
            /* video PES: flags byte, then header_data_length */
            if (len >= 3) {
                size_t hdl = s[body + 2];
                size_t payload = body + 3 + hdl;
                size_t pend = body + len;
                if (payload < pend && ps->video_cb)
                    ps->video_cb(ps->ctx, s + payload, pend - payload,
                                 pes_pts(s + body, len));
            }
        }
        else if (id == 0xBD && len >= 4) {
            /* private stream 1: first payload byte is the substream id */
            size_t hdl = s[body + 2];
            size_t payload = body + 3 + hdl;
            size_t pend = body + len;
            if (payload < pend) {
                uint8_t sub = s[payload];
                int64_t pts = pes_pts(s + body, len);
                if (sub >= 0x20 && sub <= 0x3f && ps->spu_cb)
                    ps->spu_cb(ps->ctx, sub - 0x20, s + payload + 1,
                               pend - payload - 1, pts);
                else if (sub >= 0x80 && sub <= 0x87 && ps->audio_cb
                         && pend - payload > 4)
                    /* AC-3: id, frame count, 2-byte access-unit ptr */
                    ps->audio_cb(ps->ctx, sub - 0x80, s + payload + 4,
                                 pend - payload - 4, pts);
            }
        }

        off = body + len;
    }
}
