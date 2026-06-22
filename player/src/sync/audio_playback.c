#include "sync/audio_playback.h"

#include <stdlib.h>

#include "platform/platform.h"
#include "sync/audio_resampler.h"

#define AUDIO_PRIME_MS 100
#define AUDIO_OUT_FRAMES 2048

struct pidvd_audio_playback {
    pidvd_audio_t *device;
    pidvd_audio_resampler_t *resampler;
    pidvd_av_sync_t *sync;
    uint64_t epoch;
    int rate;
    int primed_frames;
    bool started;
};

pidvd_audio_playback_t *pidvd_audio_playback_new(pidvd_av_sync_t *sync,
                                                 uint64_t epoch,
                                                 int sample_rate)
{
    pidvd_audio_playback_t *p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    p->device = pidvd_audio_open(PIDVD_AUDIO_PCM_STEREO, sample_rate);
    p->resampler = pidvd_audio_resampler_new(sample_rate);
    if (!p->device || !p->resampler) {
        pidvd_audio_playback_free(p, true);
        return NULL;
    }
    p->sync = sync;
    p->epoch = epoch;
    p->rate = sample_rate;
    return p;
}

void pidvd_audio_playback_free(pidvd_audio_playback_t *p, bool abort)
{
    if (!p)
        return;
    if (p->device) {
        if (abort)
            pidvd_audio_abort(p->device);
        else
            pidvd_audio_close(p->device);
    }
    pidvd_audio_resampler_free(p->resampler);
    free(p);
}

int pidvd_audio_playback_rate(const pidvd_audio_playback_t *p)
{
    return p ? p->rate : 0;
}

int pidvd_audio_playback_prime_frames(int sample_rate)
{
    return sample_rate * AUDIO_PRIME_MS / 1000;
}

static bool start_when_ready(pidvd_audio_playback_t *p)
{
    if (p->started
        || p->primed_frames < pidvd_audio_playback_prime_frames(p->rate))
        return true;
    pidvd_av_sync_audio_ready(p->sync, p->epoch, true);
    if (!pidvd_av_sync_wait_video_started(p->sync, p->epoch, 2000)
        || !pidvd_audio_start(p->device))
        return false;
    p->started = true;
    return true;
}

static bool update_servo(pidvd_audio_playback_t *p)
{
    if (!p->started)
        return true;
    pidvd_audio_status_t status;
    int64_t audio_pts;
    if (!pidvd_audio_status(p->device, &status)
        || !pidvd_audio_resampler_pts(p->resampler,
                                      status.played_frames, &audio_pts))
        return true;
    int ppm = pidvd_av_sync_audio_correction(
        p->sync, p->epoch, audio_pts, status.monotonic_ns, NULL);
    /* Best-effort rate nudge. A SpeexDSP filter rebuild can transiently fail
     * (e.g. allocation under memory pressure); that must not tear down the
     * whole output timeline — keep the last good rate and retry next tick. */
    pidvd_audio_resampler_set_correction(p->resampler, ppm);
    return true;
}

bool pidvd_audio_playback_write(pidvd_audio_playback_t *p,
                                const int16_t *pcm, int frames, int64_t pts)
{
    if (!p || !update_servo(p))
        return false;
    int16_t converted[AUDIO_OUT_FRAMES * 2];
    int produced = 0;
    if (!pidvd_audio_resampler_process(p->resampler, pcm, frames, pts,
                                       converted, AUDIO_OUT_FRAMES,
                                       &produced))
        return false;
    if (produced > 0
        && pidvd_audio_write(p->device, converted, produced) != produced)
        return false;
    p->primed_frames += produced;
    return start_when_ready(p);
}

bool pidvd_audio_playback_write_gap(pidvd_audio_playback_t *p,
                                    int64_t start_pts, int64_t end_pts)
{
    if (!p || end_pts <= start_pts)
        return p != NULL;
    int64_t total = (end_pts - start_pts) * p->rate / 90000;
    static const int16_t silence[1536 * 2];
    int64_t done = 0;
    while (done < total) {
        int frames = (total - done) > 1536 ? 1536 : (int)(total - done);
        int64_t pts = start_pts + done * 90000 / p->rate;
        if (!pidvd_audio_playback_write(p, silence, frames, pts))
            return false;
        done += frames;
    }
    return true;
}
