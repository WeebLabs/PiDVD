/* dvdinfo — dump a DVD ISO's structure and the output mode PiDVD would pick.
 * Host development tool; also installed on the target for diagnostics. */
#include <stdio.h>

#include "core/disc.h"

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: dvdinfo <disc.iso>\n");
        return 2;
    }

    pidvd_disc_t *d = pidvd_disc_open(argv[1]);
    if (!d)
        return 1;

    bool mixed = false;
    pidvd_standard_t std = pidvd_disc_standard(d, &mixed);

    printf("Volume        : %s\n", pidvd_disc_volume_id(d));
    printf("Regions       : ");
    uint8_t rm = pidvd_disc_region_mask(d);
    if (rm == 0xff) {
        printf("all\n");
    } else {
        for (int r = 0; r < 8; r++)
            if (rm & (1 << r))
                printf("%d ", r + 1);
        printf("\n");
    }
    printf("Titles        : %d\n", pidvd_disc_title_count(d));
    printf("Output mode   : %s%s\n", pidvd_standard_name(std),
           mixed ? "  (MIXED-STANDARD DISC)" : "");
    printf("\n");

    for (int i = 0; i < pidvd_disc_title_count(d); i++) {
        const pidvd_title_t *t = pidvd_disc_title(d, i);
        int mm = (int)(t->seconds / 60), ss = (int)t->seconds % 60;
        printf("Title %2d  VTS %2d  %3d:%02d  %dx%d %s %s%s  "
               "%d ch, %d ang\n",
               t->title_nr, t->vts_nr, mm, ss, t->width, t->height,
               pidvd_standard_name(t->standard),
               t->aspect == PIDVD_ASPECT_16_9 ? "16:9" : "4:3",
               t->letterboxed ? " LB" : "",
               t->chapters, t->angles);
        for (int a = 0; a < t->n_audio; a++)
            printf("          audio %d: %-6s %s %dch\n", a,
                   t->audio[a].format, t->audio[a].lang,
                   t->audio[a].channels);
        if (t->n_subp > 0) {
            printf("          subs   :");
            for (int s = 0; s < t->n_subp; s++)
                printf(" %s", t->subp[s].lang);
            printf("\n");
        }
    }

    pidvd_disc_close(d);
    return 0;
}
