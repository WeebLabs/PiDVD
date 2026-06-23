/* The DVD navigation engine: libdvdnav drives playback (menus, VM,
 * stills, branching); we render its block stream and react to events. */
#ifndef PIDVD_NAV_ENGINE_H
#define PIDVD_NAV_ENGINE_H

#include <stdbool.h>
#include <stdint.h>

/* Resume point for a title, exchanged with the caller across a play. On entry,
 * if restore is set the engine jumps to {title, sector} once playback starts.
 * On return, the last in-title position is written back (captured set) so the
 * caller can persist it for next time. seconds is the elapsed time for display. */
typedef struct {
    bool     restore;
    int32_t  title;
    uint32_t sector;
    int      seconds;
    bool     captured;
} pidvd_resume_t;

/* Plays the disc from its first-play PGC (menus and all), or jumps to a saved
 * position when resume->restore is set. resume may be NULL. Returns 0 on a
 * clean stop. */
int pidvd_nav_play(const char *iso_path, pidvd_resume_t *resume);

#endif
