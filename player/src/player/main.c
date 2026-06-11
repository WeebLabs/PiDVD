/* pidvd-player — appliance entry point.
 *
 * Milestone 1 (current): identify the disc, report structure and the
 * selected output standard. Next steps wire in, in order: VEC modeset
 * (platform/linux video_out), dvdnav + demux, libmpeg2 decode to the
 * frame buffer, then audio and the field scheduler.
 */
#include <stdio.h>

#include "core/disc.h"

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : NULL;
    if (!path) {
        /* TODO(m1): scan /media/usb mounts for ISOs; autoplay if exactly
         * one, else picker. For now the ISO comes from the init script. */
        fprintf(stderr, "pidvd-player: no disc given\n");
        return 2;
    }

    pidvd_disc_t *d = pidvd_disc_open(path);
    if (!d)
        return 1;

    bool mixed;
    printf("pidvd: disc '%s', %d titles, output %s%s\n",
           pidvd_disc_volume_id(d), pidvd_disc_title_count(d),
           pidvd_standard_name(pidvd_disc_standard(d, &mixed)),
           mixed ? " (mixed-standard disc)" : "");

    /* TODO(m1): pidvd_video_open(std) → VEC interlaced modeset,
     * decode first VOB sectors, present. */

    pidvd_disc_close(d);
    return 0;
}
