/* ALSA audio output. Uses the default device — a USB S/PDIF adapter
 * (card with IEC958) or the bcm2835 headphone jack, whichever ALSA
 * resolves. Blocking writes pace the audio thread; buffer sized to
 * roughly match the video ring latency. */
#include "platform/platform.h"

#include <alsa/asoundlib.h>
#include <stdbool.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

struct pidvd_audio {
    snd_pcm_t *pcm;
    int rate;
    int64_t written;
    bool started;
};

pidvd_audio_t *pidvd_audio_open(pidvd_audio_mode_t mode, int sample_rate)
{
    if (mode != PIDVD_AUDIO_PCM_STEREO)
        return NULL;   /* IEC61937 passthrough: later */
    pidvd_audio_t *a = calloc(1, sizeof(*a));
    if (snd_pcm_open(&a->pcm, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0) {
        fprintf(stderr, "audio: cannot open ALSA default device\n");
        free(a);
        return NULL;
    }
    if (snd_pcm_set_params(a->pcm, SND_PCM_FORMAT_S16_LE,
                           SND_PCM_ACCESS_RW_INTERLEAVED, 2, sample_rate,
                           1, 350000 /* ~µs, matches video ring */) < 0) {
        fprintf(stderr, "audio: set_params failed\n");
        snd_pcm_close(a->pcm);
        free(a);
        return NULL;
    }
    snd_pcm_sw_params_t *sw;
    snd_pcm_sw_params_alloca(&sw);
    snd_pcm_uframes_t boundary;
    if (snd_pcm_sw_params_current(a->pcm, sw) < 0
        || snd_pcm_sw_params_get_boundary(sw, &boundary) < 0
        || snd_pcm_sw_params_set_start_threshold(a->pcm, sw, boundary) < 0
        || snd_pcm_sw_params(a->pcm, sw) < 0
        || snd_pcm_prepare(a->pcm) < 0) {
        fprintf(stderr, "audio: cannot configure explicit start\n");
        snd_pcm_close(a->pcm);
        free(a);
        return NULL;
    }
    a->rate = sample_rate;
    fprintf(stderr, "audio: ALSA %dHz stereo\n", sample_rate);
    return a;
}

int pidvd_audio_write(pidvd_audio_t *a, const void *data, int frames)
{
    const int16_t *p = data;
    int remaining = frames;
    while (remaining > 0) {
        snd_pcm_sframes_t n = snd_pcm_writei(a->pcm, p, remaining);
        if (n == -EINTR)
            continue;
        if (n < 0)
            return -1; /* caller resets the complete output timeline */
        if (n == 0)
            continue;
        p += n * 2;
        remaining -= (int)n;
        a->written += n;
    }
    return frames;
}

bool pidvd_audio_start(pidvd_audio_t *a)
{
    if (a->started)
        return true;
    if (snd_pcm_start(a->pcm) < 0)
        return false;
    a->started = true;
    return true;
}

bool pidvd_audio_status(pidvd_audio_t *a, pidvd_audio_status_t *status)
{
    if (!a || !status)
        return false;
    snd_pcm_sframes_t delay = 0;
    if (snd_pcm_delay(a->pcm, &delay) < 0)
        return false;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    status->played_frames = a->written - delay;
    if (status->played_frames < 0)
        status->played_frames = 0;
    status->monotonic_ns =
        (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
    return true;
}

static void audio_destroy(pidvd_audio_t *a, bool drain)
{
    if (!a)
        return;
    if (drain && a->started)
        snd_pcm_drain(a->pcm);
    else
        snd_pcm_drop(a->pcm);
    snd_pcm_close(a->pcm);
    free(a);
}

void pidvd_audio_close(pidvd_audio_t *a)
{
    audio_destroy(a, true);
}

void pidvd_audio_abort(pidvd_audio_t *a)
{
    audio_destroy(a, false);
}
