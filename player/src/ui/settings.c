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
                                        "MIDNIGHT" };
static const char *const layout_v[] = { "CONSOLE", "MARQUEE", "LEDGER" };
static const char *const audio_v[]  = { "STEREO DOWNMIX", "AC-3 PASSTHROUGH" };
static const char *const dim_v[]    = { "OFF", "AFTER 5 MIN", "AFTER 15 MIN",
                                        "AFTER 30 MIN" };

static const struct {
    const char *label;
    const char *const *values;
    int n;
} rows[UI_SET_ROWS] = {
    [UI_SET_THEME]  = { "THEME",          theme_v,  4 },
    [UI_SET_LAYOUT] = { "LAYOUT",         layout_v, 3 },
    [UI_SET_AUDIO]  = { "AUDIO OUTPUT",   audio_v,  2 },
    [UI_SET_DIM]    = { "ATTRACT DIM",    dim_v,    4 },
    [UI_SET_RESCAN] = { "RESCAN CATALOG", 0,        0 },
};

static int *field(ui_settings_t *s, int row)
{
    switch (row) {
    case UI_SET_THEME:  return &s->theme;
    case UI_SET_LAYOUT: return &s->layout;
    case UI_SET_AUDIO:  return &s->audio_out;
    case UI_SET_DIM:    return &s->attract_dim;
    default:            return 0;
    }
}

const char *ui_settings_label(int row)
{
    return (row >= 0 && row < UI_SET_ROWS) ? rows[row].label : "";
}

const char *ui_settings_value(const ui_settings_t *s, int row)
{
    if (row == UI_SET_RESCAN)
        return "\xe2\x86\xb5"; /* ↵ */
    int *f = field((ui_settings_t *)s, row);
    if (!f || *f < 0 || *f >= rows[row].n)
        return "?";
    return rows[row].values[*f];
}

int ui_settings_cycle(ui_settings_t *s, int row, int dir)
{
    int *f = field(s, row);
    if (!f)
        return 0;
    *f = (*f + dir + rows[row].n) % rows[row].n;
    return 1;
}

void ui_settings_load(ui_settings_t *s)
{
    memset(s, 0, sizeof(*s));
    s->attract_dim = 2;    /* 15 min — CRT kindness by default */
    s->last_standard = 1;  /* last-disc standard for the resume shelf */

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
            if (!strcmp(line, "theme"))        s->theme = atoi(v) & 3;
            else if (!strcmp(line, "layout"))  s->layout = atoi(v) % 3;
            else if (!strcmp(line, "audio"))   s->audio_out = atoi(v) & 1;
            else if (!strcmp(line, "dim"))     s->attract_dim = atoi(v) & 3;
            else if (!strcmp(line, "last_std")) s->last_standard = atoi(v) & 1;
            else if (!strcmp(line, "last_disc"))
                snprintf(s->last_disc, sizeof(s->last_disc), "%s", v);
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
        fprintf(f, "theme=%d\nlayout=%d\naudio=%d\ndim=%d\n"
                   "last_std=%d\nlast_disc=%s\n",
                s->theme, s->layout, s->audio_out, s->attract_dim,
                s->last_standard, s->last_disc);
        fclose(f);
    }
    if (mnt) {
        (void)system("sync");
        (void)system("umount " CFG_MNT " 2>/dev/null || true");
    }
}
