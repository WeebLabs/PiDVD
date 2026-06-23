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

/* Player-selected output: dev id "" / "AUTO" auto-picks, else forces a card.
 * volume <0 leaves the card mixer untouched. Set via pidvd_audio_configure(). */
static char g_dev_id[20] = "";
static int  g_volume = -1;

static const char *card_kind(const char *drv)
{
    if (!drv) return "";
    if (strstr(drv, "USB")) return "USB";
    if (strstr(drv, "bcm2835")) return "PWM";
    if (strstr(drv, "vc4")) return "HDMI";
    return "";
}

/* First card whose ALSA id == want, or -1. */
static int card_for_id(const char *want)
{
    if (!want || !want[0])
        return -1;
    int card = -1;
    while (snd_card_next(&card) == 0 && card >= 0) {
        char hw[16];
        snprintf(hw, sizeof(hw), "hw:%d", card);
        snd_ctl_t *ctl;
        if (snd_ctl_open(&ctl, hw, 0) < 0)
            continue;
        snd_ctl_card_info_t *info;
        snd_ctl_card_info_alloca(&info);
        int hit = 0;
        if (snd_ctl_card_info(ctl, info) == 0) {
            const char *id = snd_ctl_card_info_get_id(info);
            hit = id && !strcmp(id, want);
        }
        snd_ctl_close(ctl);
        if (hit)
            return card;
    }
    return -1;
}

/* Auto pick: first USB-Audio card, else bcm2835 PWM, never HDMI. -1 if none. */
static int auto_card(void)
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
                if (usb < 0) usb = card;
            } else if (drv && strstr(drv, "bcm2835")) {
                if (pwm < 0) pwm = card;
            }
        }
        snd_ctl_close(ctl);
    }
    return usb >= 0 ? usb : pwm;
}

/* Resolve a selection (id, or AUTO/"") to a card index. */
static int resolve_card(const char *dev_id)
{
    if (dev_id && dev_id[0] && strcmp(dev_id, "AUTO") != 0) {
        int c = card_for_id(dev_id);
        if (c >= 0)
            return c;   /* configured card present */
    }
    return auto_card();  /* AUTO, or configured card vanished */
}

int pidvd_audio_list(pidvd_audio_dev_t *out, int max)
{
    int n = 0, card = -1;
    while (n < max && snd_card_next(&card) == 0 && card >= 0) {
        char hw[16];
        snprintf(hw, sizeof(hw), "hw:%d", card);
        snd_ctl_t *ctl;
        if (snd_ctl_open(&ctl, hw, 0) < 0)
            continue;
        snd_ctl_card_info_t *info;
        snd_ctl_card_info_alloca(&info);
        if (snd_ctl_card_info(ctl, info) == 0) {
            const char *id = snd_ctl_card_info_get_id(info);
            const char *nm = snd_ctl_card_info_get_name(info);
            const char *kind = card_kind(snd_ctl_card_info_get_driver(info));
            snprintf(out[n].id, sizeof(out[n].id), "%s", id ? id : "");
            snprintf(out[n].label, sizeof(out[n].label), "%s%s%s",
                     kind, kind[0] ? " " : "", nm ? nm : (id ? id : "?"));
            n++;
        }
        snd_ctl_close(ctl);
    }
    return n;
}

void pidvd_audio_configure(const char *dev_id, int volume)
{
    snprintf(g_dev_id, sizeof(g_dev_id), "%s", dev_id ? dev_id : "");
    g_volume = volume;
}

/* The card's first playback-volume mixer element, or NULL. */
static snd_mixer_elem_t *find_vol_elem(snd_mixer_t *mix)
{
    for (snd_mixer_elem_t *e = snd_mixer_first_elem(mix); e;
         e = snd_mixer_elem_next(e))
        if (snd_mixer_selem_is_active(e)
            && snd_mixer_selem_has_playback_volume(e))
            return e;
    return NULL;
}

/* Set a card's playback volume to a 0..100 percentage (rounded to nearest).
 * Volume is treated as a single preference applied TO the active card; the
 * mixer is never read back into settings, so there is no round-trip drift. */
static bool card_set_volume(int card, int pct)
{
    if (card < 0)
        return false;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    char hw[16];
    snprintf(hw, sizeof(hw), "hw:%d", card);
    snd_mixer_t *mix;
    if (snd_mixer_open(&mix, 0) < 0)
        return false;
    bool ok = false;
    if (snd_mixer_attach(mix, hw) == 0
        && snd_mixer_selem_register(mix, NULL, NULL) == 0
        && snd_mixer_load(mix) == 0) {
        snd_mixer_elem_t *e = find_vol_elem(mix);
        long mn = 0, mx = 0;
        if (e && snd_mixer_selem_get_playback_volume_range(e, &mn, &mx) == 0
            && mx > mn) {
            long v = mn + ((mx - mn) * pct + 50) / 100;   /* round to nearest */
            if (snd_mixer_selem_set_playback_volume_all(e, v) == 0)
                ok = true;
            if (snd_mixer_selem_has_playback_switch(e))
                snd_mixer_selem_set_playback_switch_all(e, pct > 0);
        }
    }
    snd_mixer_close(mix);
    return ok;
}

bool pidvd_audio_set_volume(const char *dev_id, int volume)
{
    return card_set_volume(resolve_card(dev_id), volume);
}

int pidvd_audio_adjust_volume(int delta)
{
    int base = g_volume < 0 ? 100 : g_volume;
    base += delta;
    if (base < 0) base = 0;
    if (base > 100) base = 100;
    g_volume = base;
    card_set_volume(resolve_card(g_dev_id), g_volume);
    return g_volume;
}

int pidvd_audio_volume(void)
{
    return g_volume;
}

pidvd_audio_t *pidvd_audio_open(pidvd_audio_mode_t mode, int sample_rate)
{
    if (mode != PIDVD_AUDIO_PCM_STEREO)
        return NULL;   /* IEC61937 passthrough: later */
    pidvd_audio_t *a = calloc(1, sizeof(*a));
    if (!a)
        return NULL;
    int card = resolve_card(g_dev_id);
    char dev[24];
    if (card < 0)
        snprintf(dev, sizeof(dev), "default");
    else
        snprintf(dev, sizeof(dev), "plughw:%d,0", card);
    fprintf(stderr, "audio: output device %s\n", dev);
    if (snd_pcm_open(&a->pcm, dev, SND_PCM_STREAM_PLAYBACK, 0) < 0) {
        fprintf(stderr, "audio: cannot open ALSA device %s\n", dev);
        free(a);
        return NULL;
    }
    if (g_volume >= 0)
        card_set_volume(card, g_volume);  /* apply the configured level */
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
