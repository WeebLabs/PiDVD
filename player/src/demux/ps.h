/* Minimal MPEG-2 Program Stream demuxer for DVD VOB sectors.
 * Feed 2048-byte sectors; the video elementary stream (0xE0) is handed
 * to the callback. Audio/subpicture extraction lands in milestone 2. */
#ifndef PIDVD_DEMUX_PS_H
#define PIDVD_DEMUX_PS_H

#include <stddef.h>
#include <stdint.h>

typedef void (*pidvd_es_cb)(void *ctx, const uint8_t *data, size_t len);

typedef struct {
    pidvd_es_cb video_cb;
    void *ctx;
} pidvd_ps_t;

void pidvd_ps_init(pidvd_ps_t *ps, pidvd_es_cb video_cb, void *ctx);
void pidvd_ps_push_sector(pidvd_ps_t *ps, const uint8_t sector[2048]);

#endif
