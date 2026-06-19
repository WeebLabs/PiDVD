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

typedef struct {
    uint64_t sequence;      /* DRM vblank/page-flip sequence */
    int64_t monotonic_ns;   /* physical latch time */
} pidvd_video_stamp_t;

/* Scan: playback is always interlaced (the disc's native field cadence).
 * The picker may request progressive — 240p (NTSC) / 288p (PAL) — for a
 * rock-steady, flicker-free menu. In progressive mode the caller still
 * renders a full-height (480/576) frame; the backend decimates it 2:1
 * into the half-height scanout buffer, so render code is scan-agnostic.
 * If a progressive mode isn't available, the backend falls back to
 * interlaced rather than failing. */
typedef enum {
    PIDVD_SCAN_INTERLACED,
    PIDVD_SCAN_PROGRESSIVE,
} pidvd_scan_t;

pidvd_video_t *pidvd_video_open(pidvd_standard_t std); /* interlaced */
pidvd_video_t *pidvd_video_open_mode(pidvd_standard_t std, pidvd_scan_t scan);
/* Switch standard at a VTS boundary (mixed discs). Full modeset. */
bool pidvd_video_set_standard(pidvd_video_t *v, pidvd_standard_t std);
/* Switch standard and/or scan (the picker's live settings toggle). */
bool pidvd_video_set_mode(pidvd_video_t *v, pidvd_standard_t std,
                          pidvd_scan_t scan);
/* Acquire the next back buffer to decode into. */
pidvd_frame_t *pidvd_video_begin_frame(pidvd_video_t *v);
/* Queue the frame; tff/rff come from the MPEG-2 picture flags and drive
 * field order and 3:2 cadence. Blocks per vsync pacing. stamp may be NULL. */
bool pidvd_video_present(pidvd_video_t *v, pidvd_frame_t *f,
                         bool tff, bool rff, pidvd_video_stamp_t *stamp);
void pidvd_video_close(pidvd_video_t *v);
/* diagnostic: print N successive vblank wait sequences/timings */
void pidvd_video_vblprobe(pidvd_video_t *v, int n);
/* Flip which vblank-sequence parity weaved frames latch on — i.e. invert
 * output field dominance, live, with no modeset. The absolute mapping of
 * sequence parity to the physical top field is only settled by a per-modeset
 * hardware race on the VEC (the field-lock patch pins parity *consistency*,
 * not its absolute phase), so a motion menu's field-temporal content can come
 * up field-reversed (juddery) on some plays. This is the correction. */
void pidvd_video_toggle_field_parity(pidvd_video_t *v);
/* Set the menu's composite horizontal low-pass level (0=off..8): band-limits
 * sharp UI text to composite's luma bandwidth, suppressing cross-colour on
 * text. Driven by the SETTINGS "COMP FILTER" option. */
void pidvd_video_set_hfilter(pidvd_video_t *v, unsigned level);

/* ---- audio out ------------------------------------------------------- */
typedef struct pidvd_audio pidvd_audio_t;

typedef enum {
    PIDVD_AUDIO_PCM_STEREO,   /* decoded/downmixed */
    PIDVD_AUDIO_IEC61937_AC3, /* raw AC-3 passthrough over S/PDIF */
} pidvd_audio_mode_t;

pidvd_audio_t *pidvd_audio_open(pidvd_audio_mode_t mode, int sample_rate);
int  pidvd_audio_write(pidvd_audio_t *a, const void *data, int frames);
/* Start explicitly after prebuffering; writes never auto-start the device. */
bool pidvd_audio_start(pidvd_audio_t *a);
typedef struct {
    int64_t played_frames;
    int64_t monotonic_ns;
} pidvd_audio_status_t;
bool pidvd_audio_status(pidvd_audio_t *a, pidvd_audio_status_t *status);
/* Drain at a normal end; abort discards queued samples at a discontinuity. */
void pidvd_audio_close(pidvd_audio_t *a);
void pidvd_audio_abort(pidvd_audio_t *a);

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
    PIDVD_KEY_FIELD,   /* invert output field dominance (menu judder fix) */
} pidvd_key_t;

typedef struct pidvd_input pidvd_input_t;
pidvd_input_t *pidvd_input_open(void);
pidvd_key_t pidvd_input_poll(pidvd_input_t *in); /* non-blocking */
void pidvd_input_close(pidvd_input_t *in);

#endif
