/* AC-3 decode via liba52: feed demuxed substream bytes, get interleaved
 * stereo s16 (downmixed) with the PTS of each decoded frame. */
#ifndef PIDVD_DECODE_AUDIO_A52_H
#define PIDVD_DECODE_AUDIO_A52_H

#include <stddef.h>
#include <stdint.h>

typedef struct pidvd_adec pidvd_adec_t;

/* 256*6 = 1536 stereo frames per AC-3 frame; pts in 90kHz ticks
 * (-1 if unknown), sample_rate as parsed from the stream */
typedef void (*pidvd_audio_cb)(void *ctx, const int16_t *frames,
                               int nframes, int sample_rate, int64_t pts);

pidvd_adec_t *pidvd_adec_new(pidvd_audio_cb cb, void *ctx);
void pidvd_adec_push(pidvd_adec_t *a, const uint8_t *data, size_t len,
                     int64_t pts);
void pidvd_adec_reset(pidvd_adec_t *a);
void pidvd_adec_free(pidvd_adec_t *a);

#endif
