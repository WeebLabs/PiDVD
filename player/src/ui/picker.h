/* The appliance UI loop: attract (waiting for a drive), browse,
 * settings. Owns video/input/mounting while active; returns only when
 * an ISO has been chosen, with everything closed so the nav engine can
 * take the hardware. Linux-only (mount/umount); the rendering it drives
 * is the pure code in render.c. */
#ifndef PIDVD_UI_PICKER_H
#define PIDVD_UI_PICKER_H

#include <stddef.h>

#include "ui/settings.h"

/* Blocks through attract/browse/settings until the user picks a disc.
 * iso_out receives the absolute path. now_playing (may be NULL) is the
 * display name of the last played disc for the shelf; if the user asks
 * to resume it, iso_out receives set->last_disc instead. Returns 0. */
int pidvd_picker_main(ui_settings_t *set, const char *now_playing,
                      char *iso_out, size_t cap);

#endif
