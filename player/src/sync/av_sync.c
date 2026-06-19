#include "sync/av_sync.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>

#define PTS_HZ 90000.0
#define PTS_WRAP (INT64_C(1) << 33)
#define MAX_CORRECTION_PPM 3000
#define MAX_SLEW_PPM 60

enum audio_state {
    AUDIO_PENDING,
    AUDIO_READY,
    AUDIO_UNAVAILABLE,
};

struct pidvd_av_sync {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    clockid_t cond_clock;
    uint64_t epoch;
    enum audio_state audio;
    bool video_started;

    bool display_valid;
    int64_t display_pts;
    int64_t display_ns;

    bool servo_valid;
    double filtered_error;
    double integral_error;
    int correction_ppm;
    int64_t servo_ns;
};

static int64_t deadline_ns(pidvd_av_sync_t *s, int timeout_ms)
{
    struct timespec now;
    clock_gettime(s->cond_clock, &now);
    return (int64_t)now.tv_sec * 1000000000 + now.tv_nsec
         + (int64_t)timeout_ms * 1000000;
}

static int wait_changed(pidvd_av_sync_t *s, int64_t until_ns)
{
    struct timespec until = {
        .tv_sec = until_ns / 1000000000,
        .tv_nsec = until_ns % 1000000000,
    };
    return pthread_cond_timedwait(&s->changed, &s->lock, &until);
}

/* Select the 33-bit PTS era nearest a known unwrapped reference. */
static int64_t unwrap_near(int64_t raw, int64_t reference)
{
    raw &= PTS_WRAP - 1;
    int64_t base = reference & ~(PTS_WRAP - 1);
    int64_t value = base | raw;
    if (value - reference > PTS_WRAP / 2)
        value -= PTS_WRAP;
    else if (reference - value > PTS_WRAP / 2)
        value += PTS_WRAP;
    return value;
}

pidvd_av_sync_t *pidvd_av_sync_new(void)
{
    pidvd_av_sync_t *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    pthread_mutex_init(&s->lock, NULL);
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    s->cond_clock = CLOCK_REALTIME;
#ifdef __linux__
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    s->cond_clock = CLOCK_MONOTONIC;
#endif
    pthread_cond_init(&s->changed, &attr);
    pthread_condattr_destroy(&attr);
    s->epoch = 1;
    return s;
}

void pidvd_av_sync_free(pidvd_av_sync_t *s)
{
    if (!s)
        return;
    pthread_cond_destroy(&s->changed);
    pthread_mutex_destroy(&s->lock);
    free(s);
}

void pidvd_av_sync_reset(pidvd_av_sync_t *s, uint64_t epoch)
{
    pthread_mutex_lock(&s->lock);
    s->epoch = epoch;
    s->audio = AUDIO_PENDING;
    s->video_started = false;
    s->display_valid = false;
    s->servo_valid = false;
    s->filtered_error = 0;
    s->integral_error = 0;
    s->correction_ppm = 0;
    pthread_cond_broadcast(&s->changed);
    pthread_mutex_unlock(&s->lock);
}

void pidvd_av_sync_audio_ready(pidvd_av_sync_t *s, uint64_t epoch,
                               bool available)
{
    pthread_mutex_lock(&s->lock);
    if (s->epoch == epoch) {
        s->audio = available ? AUDIO_READY : AUDIO_UNAVAILABLE;
        pthread_cond_broadcast(&s->changed);
    }
    pthread_mutex_unlock(&s->lock);
}

bool pidvd_av_sync_wait_audio_ready(pidvd_av_sync_t *s, uint64_t epoch,
                                    int timeout_ms)
{
    int64_t until = deadline_ns(s, timeout_ms);
    pthread_mutex_lock(&s->lock);
    while (s->epoch == epoch && s->audio == AUDIO_PENDING) {
        if (wait_changed(s, until) != 0)
            break;
    }
    bool ready = s->epoch == epoch && s->audio == AUDIO_READY;
    pthread_mutex_unlock(&s->lock);
    return ready;
}

bool pidvd_av_sync_wait_video_started(pidvd_av_sync_t *s, uint64_t epoch,
                                      int timeout_ms)
{
    int64_t until = deadline_ns(s, timeout_ms);
    pthread_mutex_lock(&s->lock);
    while (s->epoch == epoch && !s->video_started) {
        if (wait_changed(s, until) != 0)
            break;
    }
    bool started = s->epoch == epoch && s->video_started;
    pthread_mutex_unlock(&s->lock);
    return started;
}

bool pidvd_av_sync_video_is_started(pidvd_av_sync_t *s, uint64_t epoch)
{
    pthread_mutex_lock(&s->lock);
    bool started = s->epoch == epoch && s->video_started;
    pthread_mutex_unlock(&s->lock);
    return started;
}

bool pidvd_av_sync_display_now(pidvd_av_sync_t *s, uint64_t epoch,
                               int64_t *pts)
{
    if (!pts)
        return false;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int64_t now_ns = (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;

    pthread_mutex_lock(&s->lock);
    bool valid = s->epoch == epoch && s->display_valid;
    if (valid) {
        int64_t elapsed = now_ns - s->display_ns;
        *pts = s->display_pts;
        if (elapsed > 0)
            *pts += (int64_t)(elapsed * (PTS_HZ / 1000000000.0));
    }
    pthread_mutex_unlock(&s->lock);
    return valid;
}

void pidvd_av_sync_video_presented(pidvd_av_sync_t *s, uint64_t epoch,
                                   int64_t pts, int64_t monotonic_ns)
{
    pthread_mutex_lock(&s->lock);
    if (s->epoch != epoch) {
        pthread_mutex_unlock(&s->lock);
        return;
    }

    if (pts >= 0) {
        int64_t value = s->display_valid
            ? unwrap_near(pts, s->display_pts) : pts;
        if (s->display_valid && value <= s->display_pts) {
            int64_t elapsed = monotonic_ns - s->display_ns;
            if (elapsed > 0)
                value = s->display_pts
                      + (int64_t)(elapsed * (PTS_HZ / 1000000000.0));
        }
        s->display_pts = value;
        s->display_ns = monotonic_ns;
        s->display_valid = true;
    } else if (s->display_valid) {
        int64_t elapsed = monotonic_ns - s->display_ns;
        if (elapsed > 0) {
            s->display_pts +=
                (int64_t)(elapsed * (PTS_HZ / 1000000000.0));
            s->display_ns = monotonic_ns;
        }
    }

    if (!s->video_started) {
        s->video_started = true;
        pthread_cond_broadcast(&s->changed);
    }
    pthread_mutex_unlock(&s->lock);
}

int pidvd_av_sync_audio_correction(pidvd_av_sync_t *s, uint64_t epoch,
                                   int64_t audio_pts, int64_t monotonic_ns,
                                   int64_t *error_ticks)
{
    pthread_mutex_lock(&s->lock);
    if (s->epoch != epoch || !s->display_valid || audio_pts < 0) {
        pthread_mutex_unlock(&s->lock);
        if (error_ticks)
            *error_ticks = 0;
        return 0;
    }

    int64_t display = s->display_pts;
    int64_t elapsed = monotonic_ns - s->display_ns;
    if (elapsed > 0)
        display += (int64_t)(elapsed * (PTS_HZ / 1000000000.0));
    int64_t audio = unwrap_near(audio_pts, display);
    int64_t error = audio - display; /* positive: audio is ahead */
    if (error_ticks)
        *error_ticks = error;

    double dt = s->servo_valid
        ? (monotonic_ns - s->servo_ns) / 1000000000.0 : 0.032;
    if (dt < 0.001)
        dt = 0.001;
    if (dt > 0.250)
        dt = 0.250;

    /* A low-pass phase detector feeding a PI loop. The proportional term
     * removes residual startup phase over seconds; the integral term learns
     * the steady USB-audio/display oscillator mismatch. */
    double alpha = 1.0 - exp(-dt / 0.35);
    if (!s->servo_valid)
        s->filtered_error = error;
    else
        s->filtered_error += alpha * (error - s->filtered_error);
    s->integral_error += (s->filtered_error / PTS_HZ) * dt;
    if (s->integral_error > 0.25)
        s->integral_error = 0.25;
    else if (s->integral_error < -0.25)
        s->integral_error = -0.25;

    double requested = 1000000.0
        * (0.045 * (s->filtered_error / PTS_HZ)
           + 0.004 * s->integral_error);
    if (requested > MAX_CORRECTION_PPM)
        requested = MAX_CORRECTION_PPM;
    else if (requested < -MAX_CORRECTION_PPM)
        requested = -MAX_CORRECTION_PPM;

    int target = (int)lrint(requested);
    int delta = target - s->correction_ppm;
    if (delta > MAX_SLEW_PPM)
        delta = MAX_SLEW_PPM;
    else if (delta < -MAX_SLEW_PPM)
        delta = -MAX_SLEW_PPM;
    s->correction_ppm += delta;
    s->servo_valid = true;
    s->servo_ns = monotonic_ns;
    int correction = s->correction_ppm;
    pthread_mutex_unlock(&s->lock);
    return correction;
}
