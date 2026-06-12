/* pidvd-player — appliance entry point.
 *
 * Identifies the disc, then hands playback to the libdvdnav engine:
 * first-play PGC, menus, button navigation, stills, SPU overlays.
 * Audio and A/V sync are milestone 2b.
 */
#include <stdio.h>
#include <string.h>

#include "core/disc.h"
#include "nav/engine.h"
#include "platform/platform.h"

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : NULL;
    if (!path) {
        fprintf(stderr, "pidvd-player: no disc given\n");
        return 2;
    }

    if (!strcmp(path, "--vblprobe")) {
        pidvd_video_t *v = pidvd_video_open(PIDVD_STD_PAL);
        if (!v)
            return 1;
        pidvd_video_vblprobe(v, 40);
        pidvd_video_close(v);
        return 0;
    }

    pidvd_disc_t *d = pidvd_disc_open(path);
    if (!d)
        return 1;

    bool mixed;
    pidvd_standard_t std = pidvd_disc_standard(d, &mixed);
    printf("pidvd: disc '%s', %d titles, output %s%s\n",
           pidvd_disc_volume_id(d), pidvd_disc_title_count(d),
           pidvd_standard_name(std), mixed ? " (mixed-standard disc)" : "");

    pidvd_disc_close(d);   /* info printed; dvdnav reopens the ISO */
    return pidvd_nav_play(path);
}
