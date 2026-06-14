/* uipreview — render every picker screen to PPM on the host, so the UI
 * can be eyeballed and iterated without a Pi or a CRT. Drives the same
 * pure render.c the player uses.
 *
 *   uipreview [outdir]    writes <outdir>/<screen>-<theme>.ppm (default
 *                         ./preview), 720x576 PAL geometry
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "ui/render.h"

#define W 720
#define H 576

static ui_item_t mk_iso(const char *name, const char *volid, int std,
                        double longest, double gb, bool scanned)
{
    ui_item_t it;
    memset(&it, 0, sizeof(it));
    snprintf(it.name, sizeof(it.name), "%s", name);
    it.size = (uint64_t)(gb * 1024.0 * 1024.0 * 1024.0);
    if (!scanned)
        return it;
    it.scanned = true;
    snprintf(it.volid, sizeof(it.volid), "%s", volid);
    it.standard = std;
    it.width = 720;
    it.height = std ? 576 : 480;
    it.wide = true;
    it.region_mask = std ? 0x02 : 0x01;
    it.titles = 4;
    it.chapters = 28;
    it.longest = longest;
    it.n_audio = 2;
    snprintf(it.audio[0], sizeof(it.audio[0]), "AC-3 5.1 EN");
    snprintf(it.audio[1], sizeof(it.audio[1]), "AC-3 2.0 DE");
    snprintf(it.subs, sizeof(it.subs), "EN DE FR NL");
    return it;
}

static void write_ppm(const char *path, const uint8_t *px)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        exit(1);
    }
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) {
        const uint8_t *p = px + i * 4; /* XRGB little-endian: B G R X */
        fputc(p[2], f);
        fputc(p[1], f);
        fputc(p[0], f);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    const char *out = argc > 1 ? argv[1] : "preview";
    mkdir(out, 0755);

    static ui_item_t pool[16];
    int np = 0;
    pool[np] = mk_iso("..", "", 1, 0, 0, false);
    pool[np].is_dir = pool[np].is_parent = true;
    np++;
    pool[np] = mk_iso("Box Sets", "", 1, 0, 18.2, false);
    pool[np].is_dir = true;
    pool[np].n_items = 12;
    np++;
    pool[np++] = mk_iso("Die Hard", "DIE_HARD_SE_PAL", 1, 7967, 7.6, true);
    pool[np++] = mk_iso("Die Hard 2", "DIE_HARD_2_PAL", 1, 7080, 6.8, true);
    pool[np++] = mk_iso("Goldeneye", "GOLDENEYE_PAL", 1, 7800, 7.1, true);
    pool[np++] = mk_iso("Heat", "HEAT_PAL", 1, 9900, 7.9, true);
    pool[np++] = mk_iso("Léon", "LEON_DC_PAL", 1, 6600, 7.4, true);
    pool[np++] = mk_iso("Ronin", "RONIN_PAL", 1, 7260, 6.9, true);
    pool[np++] = mk_iso("Speed", "SPEED_NTSC", 0, 6960, 7.0, true);
    pool[np++] = mk_iso("The Long Kiss Goodnight", "TLKG_PAL", 1, 7200,
                        7.2, true);
    pool[np++] = mk_iso("True Lies", "TRUE_LIES_PAL", 1, 8460, 7.8, true);
    pool[np] = mk_iso("Under Siege", "", 1, 0, 6.5, false); /* scanning */
    np++;

    const ui_item_t *items[16];
    for (int i = 0; i < np; i++)
        items[i] = &pool[i];

    uint8_t *px = malloc((size_t)W * H * 4);
    ui_canvas_t c = { px, W, H, W * 4 };

    ui_settings_t set;
    memset(&set, 0, sizeof(set));

    ui_view_t v;
    memset(&v, 0, sizeof(v));
    v.set = &set;
    v.items = items;
    v.n_items = np;
    v.sel = 2;
    v.source = "USB";
    v.path = "Action";
    v.now_playing = "Die Hard 2";
    v.tick = 30;

    static const char *const tn[4] = { "amber-ice", "phosphor", "vfd",
                                       "midnight" };
    char path[512];

    /* console in all four themes */
    v.screen = UI_BROWSE;
    set.layout = UI_CONSOLE;
    for (int t = 0; t < 4; t++) {
        set.theme = t;
        pidvd_ui_render(&c, &v);
        snprintf(path, sizeof(path), "%s/console-%s.ppm", out, tn[t]);
        write_ppm(path, px);
    }

    set.theme = 0;
    set.layout = UI_MARQUEE;
    pidvd_ui_render(&c, &v);
    snprintf(path, sizeof(path), "%s/marquee-amber-ice.ppm", out);
    write_ppm(path, px);

    set.layout = UI_LEDGER;
    pidvd_ui_render(&c, &v);
    snprintf(path, sizeof(path), "%s/ledger-amber-ice.ppm", out);
    write_ppm(path, px);

    set.layout = UI_WIREFRAME;
    pidvd_ui_render(&c, &v);
    snprintf(path, sizeof(path), "%s/wireframe-amber-ice.ppm", out);
    write_ppm(path, px);
    set.theme = 1;  /* also phosphor, the current default theme */
    pidvd_ui_render(&c, &v);
    snprintf(path, sizeof(path), "%s/wireframe-phosphor.ppm", out);
    write_ppm(path, px);
    set.theme = 0;

    set.layout = UI_CONSOLE;
    v.screen = UI_SETTINGS;
    v.set_sel = 0;
    pidvd_ui_render(&c, &v);
    snprintf(path, sizeof(path), "%s/settings-amber-ice.ppm", out);
    write_ppm(path, px);

    v.screen = UI_ATTRACT;
    pidvd_ui_render(&c, &v);
    snprintf(path, sizeof(path), "%s/attract-amber-ice.ppm", out);
    write_ppm(path, px);

    printf("wrote previews to %s/\n", out);
    free(px);
    return 0;
}
