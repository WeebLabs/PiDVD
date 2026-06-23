/* Display-synchronized PCM output pipeline.
 *
 * Owns the ALSA device, adaptive resampler and output timeline. One instance
 * represents one uninterrupted audio-output serial and is used only by the
 * dedicated audio thread.
 */
#ifndef PIDVD_SYNC_AUDIO_PLAYBACK_H
#define PIDVD_SYNC_AUDIO_PLAYBACK_H

#include <stdbool.h>
#include <stdint.h>

#include "sync/av_sync.h"

typedef struct pidvd_audio_playback pidvd_audio_playback_t;

pidvd_audio_playback_t *pidvd_audio_playback_new(pidvd_av_sync_t *sync,
                                                 uint64_t epoch,
                                                 int sample_rate);
void pidvd_audio_playback_free(pidvd_audio_playback_t *p, bool abort);
int pidvd_audio_playback_rate(const pidvd_audio_playback_t *p);
int pidvd_audio_playback_prime_frames(int sample_rate);

bool pidvd_audio_playback_write(pidvd_audio_playback_t *p,
                                const int16_t *pcm, int frames, int64_t pts);
bool pidvd_audio_playback_write_gap(pidvd_audio_playback_t *p,
                                    int64_t start_pts, int64_t end_pts);

#endif
