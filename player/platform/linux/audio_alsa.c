/* ALSA audio output. The output device is chosen at open time: a USB audio
 * card (the user's DAC / USB S/PDIF adapter) is preferred, the bcm2835
 * headphone (PWM) jack is the fallback, and the vc4 HDMI audio is never used.
 * Blocking writes pace the audio thread; buffer sized to roughly match the
 * video ring latency. */
#include "platform/platform.h"

#include <alsa/asoundlib.h>
#include <stdbool.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct pidvd_audio {
    snd_pcm_t *pcm;
    int rate;
    int64_t written;
    bool started;
};

/* Pick the output device: prefer the first USB-Audio card, else the bcm2835
 * PWM jack; never the HDMI audio. Writes "plughw:<card>,0" into dev (plug wraps
 * the card so any S16/rate the DAC wants is handled). Falls back to "default"
 * if card enumeration turns up nothing usable. */
static void select_audio_device(char *dev, size_t cap)
{
    int usb = -1, pwm = -1, card = -1;
    while (snd_card_next(&card) == 0 && card >= 0) {
        char hw[16];
        snprintf(hw, sizeof(hw), "hw:%d", card);
        snd_ctl_t *ctl;
        if (snd_ctl_open(&ctl, hw, 0) < 0)
            continue;
        snd_ctl_card_info_t *info;
        snd_ctl_card_info_alloca(&info);
        if (snd_ctl_card_info(ctl, info) == 0) {
            const char *drv = snd_ctl_card_info_get_driver(info);
            if (drv && strstr(drv, "USB")) {
                if (usb < 0)
                    usb = card;
            } else if (drv && strstr(drv, "bcm2835")) {
                if (pwm < 0)
                    pwm = card;
            }
            /* vc4-hdmi (and anything else) is deliberately skipped. */
        }
        snd_ctl_close(ctl);
    }
    int sel = usb >= 0 ? usb : pwm;
    if (sel < 0)
        snprintf(dev, cap, "default");
    else
        snprintf(dev, cap, "plughw:%d,0", sel);
    fprintf(stderr, "audio: output device %s (%s)\n", dev,
            usb >= 0 ? "USB" : sel >= 0 ? "PWM fallback" : "ALSA default");
}

pidvd_audio_t *pidvd_audio_open(pidvd_audio_mode_t mode, int sample_rate)
{
    if (mode != PIDVD_AUDIO_PCM_STEREO)
        return NULL;   /* IEC61937 passthrough: later */
    pidvd_audio_t *a = calloc(1, sizeof(*a));
    char dev[24];
    select_audio_device(dev, sizeof(dev));
    if (snd_pcm_open(&a->pcm, dev, SND_PCM_STREAM_PLAYBACK, 0) < 0) {
        fprintf(stderr, "audio: cannot open ALSA device %s\n", dev);
        free(a);
        return NULL;
    }
    if (snd_pcm_set_params(a->pcm, SND_PCM_FORMAT_S16_LE,
                           SND_PCM_ACCESS_RW_INTERLEAVED, 2, sample_rate,
                           1, 350000 /* ~µs: deep enough to absorb the startup
                                        servo catch-up without underrunning */) < 0) {
        fprintf(stderr, "audio: set_params failed\n");
        snd_pcm_close(a->pcm);
        free(a);
        return NULL;
    }
    /* Explicit start: prebuffer the prime window, then start playback exactly
     * when video begins so the servo only has to trim sub-frame phase. The
     * start threshold must exceed any prebuffer we write (so the stream never
     * self-starts early) yet stay a sane in-range value — the bcm2835 PWM
     * driver rejects writes when it is set to the ALSA boundary sentinel. The
     * buffer size satisfies both. The stop threshold is pinned to the boundary
     * so a transient underrun free-runs instead of latching XRUN. */
    snd_pcm_uframes_t bufsz = 0, persz = 0, boundary = 0;
    snd_pcm_get_params(a->pcm, &bufsz, &persz);
    snd_pcm_sw_params_t *sw;
    snd_pcm_sw_params_alloca(&sw);
    if (snd_pcm_sw_params_current(a->pcm, sw) < 0
        || snd_pcm_sw_params_get_boundary(sw, &boundary) < 0
        || snd_pcm_sw_params_set_start_threshold(a->pcm, sw, bufsz) < 0
        || snd_pcm_sw_params_set_stop_threshold(a->pcm, sw, boundary) < 0
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
    int recoveries = 0;
    while (remaining > 0) {
        snd_pcm_sframes_t n = snd_pcm_writei(a->pcm, p, remaining);
        if (n == -EINTR)
            continue;
        if (n < 0) {
            /* An underrun while prebuffering (the bcm2835 PWM device reports
             * XRUN on writes made before the stream is started) is recoverable:
             * re-prepare and retry. A persistent fault fails the timeline. */
            if (recoveries++ < 8 && snd_pcm_recover(a->pcm, (int)n, 1) == 0)
                continue;
            fprintf(stderr, "audio: write failed: %s\n", snd_strerror((int)n));
            return -1; /* caller resets the complete output timeline */
        }
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
    /* If a full prebuffer already auto-started the stream, that is success. */
    if (snd_pcm_state(a->pcm) != SND_PCM_STATE_RUNNING
        && snd_pcm_start(a->pcm) < 0)
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
