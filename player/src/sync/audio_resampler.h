/* Streaming stereo PCM resampler plus output-frame -> source-PTS mapping. */
#ifndef PIDVD_SYNC_AUDIO_RESAMPLER_H
#define PIDVD_SYNC_AUDIO_RESAMPLER_H

#include <stdbool.h>
#include <stdint.h>

typedef struct pidvd_audio_resampler pidvd_audio_resampler_t;

pidvd_audio_resampler_t *pidvd_audio_resampler_new(int sample_rate);
void pidvd_audio_resampler_free(pidvd_audio_resampler_t *r);
bool pidvd_audio_resampler_reset(pidvd_audio_resampler_t *r, int sample_rate);
bool pidvd_audio_resampler_set_correction(pidvd_audio_resampler_t *r,
                                          int ppm);

/* Process interleaved stereo s16. out_capacity and the returned count are
 * stereo frame counts, not individual samples. */
bool pidvd_audio_resampler_process(pidvd_audio_resampler_t *r,
                                   const int16_t *in, int in_frames,
                                   int64_t input_pts,
                                   int16_t *out, int out_capacity,
                                   int *out_frames);

/* Translate an ALSA playback position into the source PTS reaching the DAC. */
bool pidvd_audio_resampler_pts(pidvd_audio_resampler_t *r,
                               int64_t output_frame, int64_t *pts);

#endif
