#include "ui/settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The boot FAT partition is mounted only for the read/write moment so
 * the appliance stays power-cut safe. */
#define CFG_MNT  "/boot"
#define CFG_FILE "/boot/pidvd.cfg"

static const char *cfg_path(int *needs_mount)
{
    const char *env = getenv("PIDVD_CFG");
    *needs_mount = env ? 0 : 1;
    return env ? env : CFG_FILE;
}

static const char *const theme_v[]  = { "AMBER & ICE", "PHOSPHOR", "VFD",
                                        "MIDNIGHT", "TERMINAL" };
static const char *const layout_v[] = { "CONSOLE", "MARQUEE", "LEDGER",
                                        "WIREFRAME" };
static const char *const dim_v[]    = { "OFF", "AFTER 5 MIN", "AFTER 15 MIN",
                                        "AFTER 30 MIN" };
/* Screensaver picker. One effect for now; the slot is here so more can be
 * added without touching the loop. Indices match the PIDVD_SAVER_* enum. */
static const char *const saver_v[]  = { "OFF", "WARP STARFIELD", "DVD LOGO" };
/* Menu composite horizontal low-pass: 0 off, 1..8 faint->strong. 4 is the
 * [1 2 1]/4 kernel; lower = sharper text / more cross-colour, higher = softer
 * text / cleaner colour. Tune to the lightest that clears the splotching. */
static const char *const filter_v[] = { "OFF", "1", "2", "3", "4",
                                        "5", "6", "7", "8" };

static const struct {
    const char *label;
    const char *const *values;
    int n;
} rows[UI_SET_ROWS] = {
    [UI_SET_THEME]  = { "THEME",          theme_v,  5 },
    [UI_SET_LAYOUT] = { "LAYOUT",         layout_v, 4 },
    [UI_SET_ADEV]   = { "AUDIO DEVICE",   0,        0 }, /* dynamic */
    [UI_SET_VOL]    = { "VOLUME",         0,        0 }, /* dynamic */
    [UI_SET_DIM]    = { "ATTRACT DIM",    dim_v,    4 },
    [UI_SET_SAVER]  = { "SCREENSAVER",    saver_v,  3 },
    [UI_SET_FILTER] = { "COMP FILTER",    filter_v, 9 },
    [UI_SET_RESCAN] = { "RESCAN CATALOG", 0,        0 },
};

static int *field(ui_settings_t *s, int row)
{
    switch (row) {
    case UI_SET_THEME:  return &s->theme;
    case UI_SET_LAYOUT: return &s->layout;
    case UI_SET_DIM:    return &s->attract_dim;
    case UI_SET_SAVER:  return &s->saver;
    case UI_SET_FILTER: return &s->comp_filter;
    default:            return 0;   /* ADEV/VOL/RESCAN handled specially */
    }
}

const char *ui_settings_label(int row)
{
    return (row >= 0 && row < UI_SET_ROWS) ? rows[row].label : "";
}

const char *ui_settings_value(const ui_settings_t *s, int row)
{
    static char buf[12];
    if (row == UI_SET_RESCAN)
        return "\xe2\x86\xb5"; /* ↵ */
    if (row == UI_SET_ADEV) {
        if (s->n_dev <= 0)
            return "AUTO";
        int i = (s->dev_sel >= 0 && s->dev_sel < s->n_dev) ? s->dev_sel : 0;
        return s->dev_label[i];
    }
    if (row == UI_SET_VOL) {
        snprintf(buf, sizeof(buf), "%d%%", s->volume);
        return buf;
    }
    int *f = field((ui_settings_t *)s, row);
    if (!f || *f < 0 || *f >= rows[row].n)
        return "?";
    return rows[row].values[*f];
}

int ui_settings_cycle(ui_settings_t *s, int row, int dir)
{
    if (row == UI_SET_ADEV) {
        if (s->n_dev <= 0)
            return 0;
        s->dev_sel = (s->dev_sel + dir + s->n_dev) % s->n_dev;
        snprintf(s->audio_dev, sizeof(s->audio_dev), "%s",
                 s->dev_id[s->dev_sel]);
        return 1;
    }
    if (row == UI_SET_VOL) {
        int v = s->volume + dir * 5;
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        if (v == s->volume)
            return 0;
        s->volume = v;
        return 1;
    }
    int *f = field(s, row);
    if (!f)
        return 0;
    *f = (*f + dir + rows[row].n) % rows[row].n;
    return 1;
}

void ui_settings_resolve_dev(ui_settings_t *s)
{
    for (int i = 0; i < s->n_dev; i++) {
        if (!strcmp(s->dev_id[i], s->audio_dev)) {
            s->dev_sel = i;
            return;
        }
    }
    /* Device absent (asleep/unplugged): show AUTO but KEEP the saved id so it
     * re-binds when the device returns. The platform's resolve already falls
     * back to auto-selection while the id is missing. */
    s->dev_sel = 0;
}

void ui_settings_load(ui_settings_t *s)
{
    memset(s, 0, sizeof(*s));
    s->theme = 4;          /* TERMINAL (index into theme_v) */
    s->layout = 3;         /* WIREFRAME (index into layout_v) */
    s->volume = 100;       /* full scale; lower in SETTINGS */
    s->attract_dim = 2;    /* 15 min — CRT kindness by default */
    s->saver = 1;          /* WARP STARFIELD on by default (PIDVD_SAVER_WARP) */
    s->comp_filter = 5;    /* composite low-pass; tune in SETTINGS (0..8) */
    s->last_standard = 1;  /* last-disc standard for the resume shelf */
    s->audio_dev[0] = 0;   /* AUTO: prefer USB, fall back to PWM */

    int mnt;
    const char *path = cfg_path(&mnt);
    if (mnt)
        (void)system("mount -t vfat /dev/mmcblk0p1 " CFG_MNT
                     " 2>/dev/null || true");
    FILE *f = fopen(path, "r");
    if (f) {
        char line[600];
        while (fgets(line, sizeof(line), f)) {
            char *eq = strchr(line, '=');
            if (!eq)
                continue;
            *eq = 0;
            char *v = eq + 1;
            v[strcspn(v, "\r\n")] = 0;
            if (!strcmp(line, "theme"))        s->theme = atoi(v) % 5;
            else if (!strcmp(line, "layout"))  s->layout = atoi(v) % 4;
            else if (!strcmp(line, "volume")) {
                s->volume = atoi(v);
                if (s->volume < 0) s->volume = 0;
                if (s->volume > 100) s->volume = 100;
            }
            else if (!strcmp(line, "adev"))
                snprintf(s->audio_dev, sizeof(s->audio_dev), "%s", v);
            else if (!strcmp(line, "dim"))     s->attract_dim = atoi(v) & 3;
            else if (!strcmp(line, "saver"))   s->saver = atoi(v) % 3;
            else if (!strcmp(line, "filter")) {
                s->comp_filter = atoi(v);
                if (s->comp_filter < 0 || s->comp_filter > 8)
                    s->comp_filter = 5;
            }
            else if (!strcmp(line, "last_std")) s->last_standard = atoi(v) & 1;
            else if (!strcmp(line, "last_disc"))
                snprintf(s->last_disc, sizeof(s->last_disc), "%s", v);
            else if (!strcmp(line, "last_title"))   s->last_title = atoi(v);
            else if (!strcmp(line, "last_sector"))  s->last_sector = atoi(v);
            else if (!strcmp(line, "last_seconds")) s->last_seconds = atoi(v);
        }
        fclose(f);
    }
    if (mnt)
        (void)system("umount " CFG_MNT " 2>/dev/null || true");
}

void ui_settings_save(const ui_settings_t *s)
{
    int mnt;
    const char *path = cfg_path(&mnt);
    if (mnt)
        (void)system("mount -t vfat /dev/mmcblk0p1 " CFG_MNT
                     " 2>/dev/null || true");
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "theme=%d\nlayout=%d\nvolume=%d\nadev=%s\ndim=%d\n"
                   "saver=%d\nfilter=%d\nlast_std=%d\nlast_disc=%s\n"
                   "last_title=%d\nlast_sector=%d\nlast_seconds=%d\n",
                s->theme, s->layout, s->volume, s->audio_dev,
                s->attract_dim, s->saver, s->comp_filter,
                s->last_standard, s->last_disc,
                s->last_title, s->last_sector, s->last_seconds);
        fclose(f);
    }
    if (mnt) {
        (void)system("sync");
        (void)system("umount " CFG_MNT " 2>/dev/null || true");
    }
}
