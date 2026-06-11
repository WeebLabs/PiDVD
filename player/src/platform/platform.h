/* Thin platform interfaces. The player core only talks to these; the Linux
 * implementation (KMS/DRM, ALSA, evdev) lives in platform/linux/, and a
 * future bare-metal Circle port would implement the same contracts. */
#ifndef PIDVD_PLATFORM_H
#define PIDVD_PLATFORM_H

#include <stdbool.h>
#include <stdint.h>

#include "core/disc.h"

/* ---- video out -------------------------------------------------------
 * Native interlaced scanout only. The implementation owns mode setting
 * (VEC composite or DPI/VGA666) and vsync pacing. One buffer = one full
 * frame (both fields, weaved); presentation granularity is the field. */
typedef struct pidvd_video pidvd_video_t;

typedef struct {
    uint8_t *pixels;   /* XRGB8888, weaved frame */
    int      width;    /* 720 */
    int      height;   /* 480 or 576 */
    int      stride;
} pidvd_frame_t;

pidvd_video_t *pidvd_video_open(pidvd_standard_t std);
/* Switch standard at a VTS boundary (mixed discs). Full modeset. */
bool pidvd_video_set_standard(pidvd_video_t *v, pidvd_standard_t std);
/* Acquire the next back buffer to decode into. */
pidvd_frame_t *pidvd_video_begin_frame(pidvd_video_t *v);
/* Queue the frame; tff/rff come from the MPEG-2 picture flags and drive
 * field order and 3:2 cadence. Blocks per vsync pacing. */
bool pidvd_video_present(pidvd_video_t *v, pidvd_frame_t *f,
                         bool tff, bool rff);
void pidvd_video_close(pidvd_video_t *v);

/* ---- audio out ------------------------------------------------------- */
typedef struct pidvd_audio pidvd_audio_t;

typedef enum {
    PIDVD_AUDIO_PCM_STEREO,   /* decoded/downmixed */
    PIDVD_AUDIO_IEC61937_AC3, /* raw AC-3 passthrough over S/PDIF */
} pidvd_audio_mode_t;

pidvd_audio_t *pidvd_audio_open(pidvd_audio_mode_t mode, int sample_rate);
int  pidvd_audio_write(pidvd_audio_t *a, const void *data, int frames);
/* Monotonic playback position in samples — the master A/V clock. */
int64_t pidvd_audio_position(pidvd_audio_t *a);
void pidvd_audio_close(pidvd_audio_t *a);

/* ---- input ------------------------------------------------------------
 * All devices (USB keyboard, 2.4 GHz HID remotes, gpio-ir) normalize to
 * these events. */
typedef enum {
    PIDVD_KEY_NONE = 0,
    PIDVD_KEY_UP, PIDVD_KEY_DOWN, PIDVD_KEY_LEFT, PIDVD_KEY_RIGHT,
    PIDVD_KEY_ENTER, PIDVD_KEY_MENU, PIDVD_KEY_TITLE,
    PIDVD_KEY_PLAY_PAUSE, PIDVD_KEY_STOP,
    PIDVD_KEY_PREV_CHAPTER, PIDVD_KEY_NEXT_CHAPTER,
    PIDVD_KEY_AUDIO, PIDVD_KEY_SUBTITLE, PIDVD_KEY_ANGLE,
} pidvd_key_t;

typedef struct pidvd_input pidvd_input_t;
pidvd_input_t *pidvd_input_open(void);
pidvd_key_t pidvd_input_poll(pidvd_input_t *in); /* non-blocking */
void pidvd_input_close(pidvd_input_t *in);

#endif
