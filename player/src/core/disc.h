/* PiDVD disc model: open a DVD-Video ISO, expose IFO-derived facts.
 * Everything the player needs to choose an output mode lives here —
 * the video standard comes from the IFO video attributes, never the
 * region code. */
#ifndef PIDVD_CORE_DISC_H
#define PIDVD_CORE_DISC_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    PIDVD_STD_NTSC, /* 720x480i, 59.94 fields/s */
    PIDVD_STD_PAL,  /* 720x576i, 50 fields/s    */
} pidvd_standard_t;

typedef enum {
    PIDVD_ASPECT_4_3,
    PIDVD_ASPECT_16_9, /* anamorphic; signaled, never scaled */
} pidvd_aspect_t;

typedef struct {
    char     format[16];   /* "AC-3", "LPCM", "MPEG-2", "DTS", ... */
    char     lang[4];      /* ISO 639 code or "--" */
    uint8_t  channels;
    uint8_t  phys;         /* physical PES stream id (0..7) */
} pidvd_audio_stream_t;

typedef struct {
    char    lang[4];
    uint8_t phys;   /* physical PES substream id (0..31) for this title's aspect */
} pidvd_subp_stream_t;

#define PIDVD_MAX_AUDIO 8
#define PIDVD_MAX_SUBP  32

typedef struct {
    int      title_nr;      /* 1-based, disc-wide */
    int      vts_nr;        /* title set holding this title */
    int      chapters;
    int      angles;
    double   seconds;       /* main PGC playback time, 0 if unknown */
    pidvd_standard_t standard;
    pidvd_aspect_t   aspect;
    int      width, height; /* coded picture size */
    bool     letterboxed;
    int      n_audio;
    pidvd_audio_stream_t audio[PIDVD_MAX_AUDIO];
    int      n_subp;
    pidvd_subp_stream_t  subp[PIDVD_MAX_SUBP];
} pidvd_title_t;

typedef struct pidvd_disc pidvd_disc_t;

/* Open an ISO image (or device/directory — anything libdvdread accepts).
 * Returns NULL with a message on stderr on failure. */
pidvd_disc_t *pidvd_disc_open(const char *path);
void pidvd_disc_close(pidvd_disc_t *d);

const char *pidvd_disc_volume_id(const pidvd_disc_t *d);
/* Allowed-region bitmask from the VMG category word: bit 0 = region 1 …
 * bit 7 = region 8. 0xff = all regions (region-free). */
uint8_t pidvd_disc_region_mask(const pidvd_disc_t *d);
int pidvd_disc_title_count(const pidvd_disc_t *d);
const pidvd_title_t *pidvd_disc_title(const pidvd_disc_t *d, int idx);

/* The standard the output should be programmed to: the standard of the
 * first/longest title's VTS. `mixed` is set if any VTS disagrees (legal
 * but rare; presenter must then switch at VTS boundaries). */
pidvd_standard_t pidvd_disc_standard(const pidvd_disc_t *d, bool *mixed);

const char *pidvd_standard_name(pidvd_standard_t s);

/* The underlying dvd_reader_t* for VOB access (cast at the call site;
 * kept void* so this header stays free of libdvdread types).
 * TODO(m2): replaced by the dvdnav-based stream layer. */
void *pidvd_disc_reader(const pidvd_disc_t *d);

#endif
