/* pidvd-player — appliance entry point.
 *
 * UI mode (no args or --ui): the picker owns the screen — attract until
 * a USB drive appears, browse/settings, then playback via the libdvdnav
 * engine; stop returns to the picker. A single ISO path argument plays
 * that disc directly (development / legacy init). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "core/disc.h"
#include "nav/engine.h"
#include "platform/platform.h"
#include "ui/picker.h"
#include "ui/settings.h"

static void display_name(const char *path, char *out, int cap)
{
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    snprintf(out, (size_t)cap, "%s", base);
    size_t n = strlen(out);
    if (n > 4 && !strcasecmp(out + n - 4, ".iso"))
        out[n - 4] = 0;
    for (char *p = out; *p; p++)
        if (*p == '_')
            *p = ' ';
}

static int play_once(const char *path)
{
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

static int ui_main(void)
{
    ui_settings_t set;
    ui_settings_load(&set);

    static char iso[1024], now_buf[96];
    const char *now_name = NULL;

    for (;;) {
        if (pidvd_picker_main(&set, now_name, iso, sizeof(iso)) != 0)
            return 1;

        pidvd_disc_t *d = pidvd_disc_open(iso);
        if (d) {
            bool mixed;
            set.last_standard =
                pidvd_disc_standard(d, &mixed) == PIDVD_STD_PAL;
            pidvd_disc_close(d);
        }
        snprintf(set.last_disc, sizeof(set.last_disc), "%s", iso);
        ui_settings_save(&set);

        /* the engine picks this up when passthrough lands (milestone 2b+) */
        setenv("PIDVD_AUDIO", set.audio_out ? "passthrough" : "stereo", 1);

        display_name(iso, now_buf, sizeof(now_buf));
        printf("pidvd: playing %s\n", iso);
        pidvd_nav_play(iso);
        now_name = now_buf;
    }
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : NULL;

    if (path && !strcmp(path, "--vblprobe")) {
        pidvd_video_t *v = pidvd_video_open(PIDVD_STD_PAL);
        if (!v)
            return 1;
        pidvd_video_vblprobe(v, 40);
        pidvd_video_close(v);
        return 0;
    }

    if (!path || !strcmp(path, "--ui"))
        return ui_main();

    return play_once(path);
}
