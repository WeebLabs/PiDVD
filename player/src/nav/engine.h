/* The DVD navigation engine: libdvdnav drives playback (menus, VM,
 * stills, branching); we render its block stream and react to events. */
#ifndef PIDVD_NAV_ENGINE_H
#define PIDVD_NAV_ENGINE_H

/* Plays the disc from its first-play PGC (menus and all). Returns 0 on
 * a clean stop. */
int pidvd_nav_play(const char *iso_path);

#endif
