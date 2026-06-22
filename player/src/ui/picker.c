#include "ui/picker.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "platform/platform.h"
#include "ui/catalog.h"
#include "ui/render.h"

#define MEDIA_ROOT "/media/usb"
#define MAX_VIEW 2048

/* ---- drive mounting ----------------------------------------------------
 * The player owns the "disc tray": busybox mount autodetects the fs
 * (exFAT expected — FAT32 can't hold DVD9 ISOs). rw so the catalog
 * cache can live on the drive; cache writes degrade gracefully on ro
 * media. */

static bool drive_mounted(void)
{
    struct stat a, b;
    if (stat(MEDIA_ROOT, &a) || stat("/media", &b))
        return false;
    return a.st_dev != b.st_dev;
}

static bool drive_try_mount(void)
{
    if (drive_mounted())
        return true;
    mkdir("/media", 0755);
    mkdir(MEDIA_ROOT, 0755);
    static const char *const devs[] = { "/dev/sda1", "/dev/sda" };
    for (unsigned i = 0; i < 2; i++) {
        struct stat st;
        if (stat(devs[i], &st) || !S_ISBLK(st.st_mode))
            continue;
        char cmd[128];
        snprintf(cmd, sizeof(cmd),
                 "mount -o rw,noatime %s " MEDIA_ROOT " 2>/dev/null",
                 devs[i]);
        if (system(cmd) == 0 && drive_mounted())
            return true;
    }
    return false;
}

static void drive_eject(void)
{
    (void)system("umount " MEDIA_ROOT " 2>/dev/null");
}

/* ---- the loop ---------------------------------------------------------- */

/* The menu is 240p NTSC — progressive, the steady retro look. Earlier
 * "240p rolls over composite" turned out to be the VEC mis-programming
 * bug (legacy/partial modeset left the encoder half-set); with the atomic
 * off->on modeset in video_kms the VEC is fully re-initialised and 240p is
 * clean. Selecting a disc switches to its native 576i/480i (atomic off->on
 * again), with playback staying field-accurate via the unchanged
 * interlaced flip path; STOP returns here and re-enters 240p NTSC. The
 * menu canvas renders at 720x480 and is decimated 2:1 to 720x240. */
#define MENU_STD  PIDVD_STD_NTSC
#define MENU_ROWS_H 480   /* logical render height; decimated 2:1 to 240p */

typedef struct {
    pidvd_video_t *video;
} vout_t;

static void present(vout_t *vo, const ui_view_t *view)
{
    pidvd_frame_t *f = pidvd_video_begin_frame(vo->video);
    if (!f)
        return;
    ui_canvas_t c = { f->pixels, f->width, f->height, f->stride };
    pidvd_ui_render(&c, view);
    pidvd_video_present(vo->video, f, true, false, NULL);
}

int pidvd_picker_main(ui_settings_t *set, const char *now_playing,
                      char *iso_out, size_t cap)
{
    vout_t vo;
    vo.video = pidvd_video_open_mode(MENU_STD, PIDVD_SCAN_PROGRESSIVE);
    if (!vo.video)
        return -1;
    pidvd_video_set_hfilter(vo.video, (unsigned)set->comp_filter);
    pidvd_input_t *in = pidvd_input_open();

    static const ui_item_t *view_items[MAX_VIEW];
    ui_view_t view;
    memset(&view, 0, sizeof(view));
    view.set = set;
    view.source = "USB";
    view.now_playing = now_playing;

    /* Refresh the selectable audio outputs each time the menu opens, so a DAC
     * plugged in between sessions appears. Index 0 is always AUTO. */
    pidvd_audio_dev_t devs[UI_AUDIO_DEV_MAX - 1];
    int nd = pidvd_audio_list(devs, UI_AUDIO_DEV_MAX - 1);
    set->dev_id[0][0] = 0;
    snprintf(set->dev_label[0], sizeof(set->dev_label[0]), "AUTO");
    set->n_dev = 1;
    for (int i = 0; i < nd && set->n_dev < UI_AUDIO_DEV_MAX; i++, set->n_dev++) {
        snprintf(set->dev_id[set->n_dev], sizeof(set->dev_id[0]),
                 "%s", devs[i].id);
        snprintf(set->dev_label[set->n_dev], sizeof(set->dev_label[0]),
                 "%s", devs[i].label);
    }
    ui_settings_resolve_dev(set);
    /* Push the saved volume to the active card so the displayed % matches the
     * hardware (same "config wins" rule playback uses). */
    pidvd_audio_set_volume(set->audio_dev, set->volume);

    catalog_t *cat = NULL;
    bool autoplay_armed = (now_playing == NULL);
    int result = -1;
    ui_screen_t prev_screen = UI_ATTRACT;

    while (result < 0) {
        /* mount state drives attract<->browse */
        if (!cat) {
            if ((view.tick % 25) == 0 && drive_try_mount()) {
                cat = catalog_open(MEDIA_ROOT);
                view.screen = UI_BROWSE;
                view.sel = view.scroll = 0;
                char only[1024];
                if (autoplay_armed
                    && catalog_root_iso_count(cat, only, sizeof(only)) == 1) {
                    snprintf(iso_out, cap, "%s", only);
                    result = 0;
                    break;
                }
                autoplay_armed = false;
            } else if (view.screen == UI_BROWSE) {
                view.screen = UI_ATTRACT;
            }
        } else if (!drive_mounted()) {
            /* yanked mid-browse */
            catalog_close(cat);
            cat = NULL;
            view.screen = UI_ATTRACT;
        }

        pidvd_key_t k = pidvd_input_poll(in);

        if (view.screen == UI_SETTINGS) {
            switch (k) {
            case PIDVD_KEY_UP:
                view.set_sel = (view.set_sel + UI_SET_ROWS - 1) % UI_SET_ROWS;
                break;
            case PIDVD_KEY_DOWN:
                view.set_sel = (view.set_sel + 1) % UI_SET_ROWS;
                break;
            case PIDVD_KEY_LEFT:
                ui_settings_cycle(set, view.set_sel, -1);
                break;
            case PIDVD_KEY_RIGHT:
                ui_settings_cycle(set, view.set_sel, +1);
                break;
            case PIDVD_KEY_ENTER:
                if (view.set_sel == UI_SET_RESCAN && cat)
                    catalog_rescan(cat);
                else
                    ui_settings_cycle(set, view.set_sel, +1);
                break;
            case PIDVD_KEY_MENU:
            case PIDVD_KEY_STOP:
                ui_settings_save(set);
                view.screen = prev_screen;
                break;
            default:
                break;
            }
            /* COMP FILTER applies live on the CRT as it's cycled. */
            pidvd_video_set_hfilter(vo.video, (unsigned)set->comp_filter);
            /* VOLUME is a single preference applied to the active card. Pushing
             * it on a volume change (live) or a device change (so the newly
             * selected card adopts it) keeps hardware and the displayed % in
             * step without ever reading the mixer back into the setting. */
            if ((k == PIDVD_KEY_LEFT || k == PIDVD_KEY_RIGHT
                 || k == PIDVD_KEY_ENTER)
                && (view.set_sel == UI_SET_VOL || view.set_sel == UI_SET_ADEV))
                pidvd_audio_set_volume(set->audio_dev, set->volume);
        } else if (view.screen == UI_BROWSE && cat) {
            catalog_lock(cat);
            int n = catalog_count(cat);
            catalog_unlock(cat);
            int rows = pidvd_ui_visible_rows(&view, MENU_ROWS_H);
            if (rows < 1)
                rows = 1;
            bool wrap = (set->layout == UI_MARQUEE);

            switch (k) {
            case PIDVD_KEY_UP:
                if (view.sel > 0) view.sel--;
                else if (wrap && n) view.sel = n - 1;
                break;
            case PIDVD_KEY_DOWN:
                if (view.sel < n - 1) view.sel++;
                else if (wrap) view.sel = 0;
                break;
            case PIDVD_KEY_PREV_CHAPTER:
                view.sel -= rows;
                if (view.sel < 0) view.sel = 0;
                break;
            case PIDVD_KEY_NEXT_CHAPTER:
                view.sel += rows;
                if (view.sel > n - 1) view.sel = n - 1;
                break;
            case PIDVD_KEY_RIGHT:
            case PIDVD_KEY_ENTER:
            case PIDVD_KEY_PLAY_PAUSE: {
                catalog_lock(cat);
                const cat_entry_t *e = catalog_get(cat, view.sel);
                bool parent = e && e->v.is_parent;
                bool dir = e && e->v.is_dir && !parent;
                if (e && !e->v.is_dir)
                    catalog_abspath(cat, e, iso_out, (int)cap);
                catalog_unlock(cat);
                if (parent) {
                    catalog_up(cat);
                    view.sel = view.scroll = 0;
                } else if (dir) {
                    catalog_enter(cat, view.sel);
                    view.sel = view.scroll = 0;
                } else if (e) {
                    result = 0;
                }
                break;
            }
            case PIDVD_KEY_LEFT:
            case PIDVD_KEY_MENU:
                if (!catalog_at_root(cat)) {
                    catalog_up(cat);
                    view.sel = view.scroll = 0;
                } else {
                    prev_screen = UI_BROWSE;
                    view.screen = UI_SETTINGS;
                    view.set_sel = 0;
                }
                break;
            case PIDVD_KEY_TITLE:
                if (now_playing && set->last_disc[0]) {
                    snprintf(iso_out, cap, "%s", set->last_disc);
                    result = 0;
                }
                break;
            case PIDVD_KEY_STOP:
                catalog_close(cat);
                cat = NULL;
                drive_eject();
                view.screen = UI_ATTRACT;
                view.notice = NULL;
                break;
            default:
                break;
            }

            if (view.sel < 0) view.sel = 0;
            if (view.sel > n - 1) view.sel = n > 0 ? n - 1 : 0;
            if (view.sel < view.scroll)
                view.scroll = view.sel;
            if (view.sel >= view.scroll + rows)
                view.scroll = view.sel - rows + 1;
        } else { /* ATTRACT */
            if (k == PIDVD_KEY_MENU) {
                prev_screen = UI_ATTRACT;
                view.screen = UI_SETTINGS;
                view.set_sel = 0;
            }
        }

        /* build the item view + render under the catalog lock */
        if (cat && view.screen == UI_BROWSE) {
            catalog_lock(cat);
            int n = catalog_count(cat);
            if (n > MAX_VIEW)
                n = MAX_VIEW;
            for (int i = 0; i < n; i++)
                view_items[i] = &catalog_get(cat, i)->v;
            view.items = view_items;
            view.n_items = n;
            view.at_root = catalog_at_root(cat);
            view.path = catalog_cwd(cat);
            view.scanning = catalog_scanning(cat);
            present(&vo, &view);
            catalog_unlock(cat);
        } else {
            view.items = NULL;
            view.n_items = 0;
            present(&vo, &view);
        }
        view.tick++;
    }

    if (cat)
        catalog_close(cat);
    pidvd_input_close(in);
    pidvd_video_close(vo.video);
    return result;
}
