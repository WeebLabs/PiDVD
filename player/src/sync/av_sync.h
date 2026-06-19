/* Display-master A/V synchronization.
 *
 * Video presentation is never altered for synchronization. Successful KMS
 * flips publish the display clock here; the audio thread compares the PCM
 * sample currently reaching the DAC against that clock and receives a small,
 * slew-limited resampling correction.
 */
#ifndef PIDVD_SYNC_AV_SYNC_H
#define PIDVD_SYNC_AV_SYNC_H

#include <stdbool.h>
#include <stdint.h>

typedef struct pidvd_av_sync pidvd_av_sync_t;

pidvd_av_sync_t *pidvd_av_sync_new(void);
void pidvd_av_sync_free(pidvd_av_sync_t *s);

/* Begin an unrelated PTS epoch (seek, branch, VTS or audio-stream change). */
void pidvd_av_sync_reset(pidvd_av_sync_t *s, uint64_t epoch);

/* Startup barrier. The first frame of an epoch waits briefly for PCM to be
 * primed, then starts even if audio is absent or failed. */
void pidvd_av_sync_audio_ready(pidvd_av_sync_t *s, uint64_t epoch,
                               bool available);
bool pidvd_av_sync_wait_audio_ready(pidvd_av_sync_t *s, uint64_t epoch,
                                    int timeout_ms);
bool pidvd_av_sync_wait_video_started(pidvd_av_sync_t *s, uint64_t epoch,
                                      int timeout_ms);
bool pidvd_av_sync_video_is_started(pidvd_av_sync_t *s, uint64_t epoch);
bool pidvd_av_sync_display_now(pidvd_av_sync_t *s, uint64_t epoch,
                               int64_t *pts);

/* Publish the PTS physically placed on screen at monotonic_ns. */
void pidvd_av_sync_video_presented(pidvd_av_sync_t *s, uint64_t epoch,
                                   int64_t pts, int64_t monotonic_ns);

/* Return the requested output-duration correction in parts per million.
 * Positive values produce more PCM frames (slow audio); negative values
 * produce fewer (speed audio). */
int pidvd_av_sync_audio_correction(pidvd_av_sync_t *s, uint64_t epoch,
                                   int64_t audio_pts, int64_t monotonic_ns,
                                   int64_t *error_ticks);

#endif
