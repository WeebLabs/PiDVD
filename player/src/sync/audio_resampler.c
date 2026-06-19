#include "sync/audio_resampler.h"

#include <math.h>
#include <speex/speex_resampler.h>
#include <stdlib.h>

#define CHANNELS 2
#define QUALITY 8
#define RATIO_SCALE 1000000
#define TIMELINE_SEGMENTS 256

struct timeline_segment {
    int64_t first;
    int64_t end;
    double first_pts;
    double ticks_per_frame;
};

struct pidvd_audio_resampler {
    SpeexResamplerState *state;
    int sample_rate;
    int correction_ppm;
    int64_t output_cursor;
    double next_output_pts;
    bool timeline_valid;
    struct timeline_segment timeline[TIMELINE_SEGMENTS];
    unsigned timeline_head;
    unsigned timeline_count;
};

static void clear_timeline(pidvd_audio_resampler_t *r)
{
    r->output_cursor = 0;
    r->next_output_pts = 0;
    r->timeline_valid = false;
    r->timeline_head = 0;
    r->timeline_count = 0;
}

pidvd_audio_resampler_t *pidvd_audio_resampler_new(int sample_rate)
{
    pidvd_audio_resampler_t *r = calloc(1, sizeof(*r));
    if (!r)
        return NULL;
    if (!pidvd_audio_resampler_reset(r, sample_rate)) {
        pidvd_audio_resampler_free(r);
        return NULL;
    }
    return r;
}

void pidvd_audio_resampler_free(pidvd_audio_resampler_t *r)
{
    if (!r)
        return;
    if (r->state)
        speex_resampler_destroy(r->state);
    free(r);
}

bool pidvd_audio_resampler_reset(pidvd_audio_resampler_t *r, int sample_rate)
{
    if (sample_rate <= 0)
        return false;
    if (r->state)
        speex_resampler_destroy(r->state);
    int err = RESAMPLER_ERR_SUCCESS;
    r->state = speex_resampler_init(CHANNELS, (spx_uint32_t)sample_rate,
                                    (spx_uint32_t)sample_rate, QUALITY, &err);
    if (!r->state || err != RESAMPLER_ERR_SUCCESS)
        return false;
    /* This is finite, prebuffered programme audio rather than a live capture:
     * remove filter startup zeros so the first output sample retains its PTS. */
    if (speex_resampler_skip_zeros(r->state) != RESAMPLER_ERR_SUCCESS) {
        speex_resampler_destroy(r->state);
        r->state = NULL;
        return false;
    }
    r->sample_rate = sample_rate;
    r->correction_ppm = 0;
    clear_timeline(r);
    return true;
}

bool pidvd_audio_resampler_set_correction(pidvd_audio_resampler_t *r,
                                          int ppm)
{
    if (ppm == r->correction_ppm)
        return true;
    int output_scale = RATIO_SCALE + ppm;
    if (output_scale <= 0)
        return false;
    int output_rate = (int)lrint(r->sample_rate
                               * (output_scale / (double)RATIO_SCALE));
    int err = speex_resampler_set_rate_frac(
        r->state, RATIO_SCALE, (spx_uint32_t)output_scale,
        (spx_uint32_t)r->sample_rate, (spx_uint32_t)output_rate);
    if (err != RESAMPLER_ERR_SUCCESS)
        return false;
    r->correction_ppm = ppm;
    return true;
}

static void add_segment(pidvd_audio_resampler_t *r, int frames,
                        double ticks_per_frame)
{
    if (frames <= 0)
        return;
    unsigned slot = r->timeline_head;
    r->timeline[slot] = (struct timeline_segment) {
        .first = r->output_cursor,
        .end = r->output_cursor + frames,
        .first_pts = r->next_output_pts,
        .ticks_per_frame = ticks_per_frame,
    };
    r->timeline_head = (slot + 1) % TIMELINE_SEGMENTS;
    if (r->timeline_count < TIMELINE_SEGMENTS)
        r->timeline_count++;
    r->output_cursor += frames;
    r->next_output_pts += frames * ticks_per_frame;
}

bool pidvd_audio_resampler_process(pidvd_audio_resampler_t *r,
                                   const int16_t *in, int in_frames,
                                   int64_t input_pts,
                                   int16_t *out, int out_capacity,
                                   int *out_frames)
{
    if (!r || !r->state || !in || in_frames < 0 || !out
        || out_capacity <= 0 || !out_frames)
        return false;

    if (!r->timeline_valid) {
        if (input_pts < 0)
            return false;
        r->next_output_pts = input_pts;
        r->timeline_valid = true;
    } else if (input_pts >= 0) {
        /* Explicit PES timestamps are authoritative at discontinuities, while
         * ordinary sub-tick rounding noise must not fracture the timeline. */
        double difference = input_pts - r->next_output_pts;
        if (fabs(difference) > 9000.0)
            r->next_output_pts = input_pts;
    }

    spx_uint32_t consumed = (spx_uint32_t)in_frames;
    spx_uint32_t produced = (spx_uint32_t)out_capacity;
    int err = speex_resampler_process_interleaved_int(
        r->state, in, &consumed, out, &produced);
    if (err != RESAMPLER_ERR_SUCCESS
        || consumed != (spx_uint32_t)in_frames)
        return false;

    double stretch = (RATIO_SCALE + r->correction_ppm)
                   / (double)RATIO_SCALE;
    double ticks = 90000.0 / (r->sample_rate * stretch);
    add_segment(r, (int)produced, ticks);
    *out_frames = (int)produced;
    return true;
}

bool pidvd_audio_resampler_pts(pidvd_audio_resampler_t *r,
                               int64_t output_frame, int64_t *pts)
{
    if (!r || !pts || r->timeline_count == 0)
        return false;
    unsigned oldest = (r->timeline_head + TIMELINE_SEGMENTS
                       - r->timeline_count) % TIMELINE_SEGMENTS;
    for (unsigned i = 0; i < r->timeline_count; i++) {
        const struct timeline_segment *s =
            &r->timeline[(oldest + i) % TIMELINE_SEGMENTS];
        if (output_frame >= s->first && output_frame < s->end) {
            *pts = (int64_t)llround(s->first_pts
                                  + (output_frame - s->first)
                                    * s->ticks_per_frame);
            return true;
        }
    }
    const struct timeline_segment *last =
        &r->timeline[(r->timeline_head + TIMELINE_SEGMENTS - 1)
                     % TIMELINE_SEGMENTS];
    if (output_frame == last->end) {
        *pts = (int64_t)llround(last->first_pts
                              + (last->end - last->first)
                                * last->ticks_per_frame);
        return true;
    }
    return false;
}
