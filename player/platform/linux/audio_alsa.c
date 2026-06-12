/* ALSA audio output. Uses the default device — a USB S/PDIF adapter
 * (card with IEC958) or the bcm2835 headphone jack, whichever ALSA
 * resolves. Blocking writes pace the audio thread; buffer sized to
 * roughly match the video ring latency. */
#include "platform/platform.h"

#include <alsa/asoundlib.h>
#include <stdio.h>
#include <stdlib.h>

struct pidvd_audio {
    snd_pcm_t *pcm;
    int rate;
    int64_t written;
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
    a->rate = sample_rate;
    fprintf(stderr, "audio: ALSA %dHz stereo\n", sample_rate);
    return a;
}

int pidvd_audio_write(pidvd_audio_t *a, const void *data, int frames)
{
    snd_pcm_sframes_t n = snd_pcm_writei(a->pcm, data, frames);
    if (n < 0)
        n = snd_pcm_recover(a->pcm, (int)n, 1) == 0
            ? snd_pcm_writei(a->pcm, data, frames) : -1;
    if (n > 0)
        a->written += n;
    return (int)n;
}

int64_t pidvd_audio_position(pidvd_audio_t *a)
{
    snd_pcm_sframes_t delay = 0;
    snd_pcm_delay(a->pcm, &delay);
    int64_t pos = a->written - delay;
    return pos < 0 ? 0 : pos;
}

void pidvd_audio_close(pidvd_audio_t *a)
{
    if (!a)
        return;
    snd_pcm_drain(a->pcm);
    snd_pcm_close(a->pcm);
    free(a);
}
