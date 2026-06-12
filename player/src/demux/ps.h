/* Minimal MPEG-2 Program Stream demuxer for DVD VOB sectors.
 * Feed 2048-byte sectors; the video elementary stream (0xE0) is handed
 * to the callback. Audio/subpicture extraction lands in milestone 2. */
#ifndef PIDVD_DEMUX_PS_H
#define PIDVD_DEMUX_PS_H

#include <stddef.h>
#include <stdint.h>

/* pts is 90kHz PES PTS, -1 when the packet carries none */
typedef void (*pidvd_es_cb)(void *ctx, const uint8_t *data, size_t len,
                            int64_t pts);
typedef void (*pidvd_sub_cb)(void *ctx, int substream, const uint8_t *data,
                             size_t len, int64_t pts);

typedef struct {
    pidvd_es_cb video_cb;
    pidvd_sub_cb spu_cb;     /* sub-picture substreams 0-31 */
    pidvd_sub_cb audio_cb;   /* AC-3 substreams 0-7 (0x80+n) */
    void *ctx;
} pidvd_ps_t;

void pidvd_ps_init(pidvd_ps_t *ps, pidvd_es_cb video_cb, void *ctx);
void pidvd_ps_push_sector(pidvd_ps_t *ps, const uint8_t sector[2048]);

#endif
