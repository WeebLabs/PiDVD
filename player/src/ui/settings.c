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
                                        "MIDNIGHT", "TERMINAL", "DARK SAKURA",
                                        "LIGHT SAKURA" };
static const char *const layout_v[] = { "CONSOLE", "MARQUEE", "LEDGER",
                                        "WIREFRAME" };
static const char *const dim_v[]    = { "OFF", "AFTER 5 MIN", "AFTER 15 MIN",
                                        "AFTER 30 MIN" };
/* Screensaver picker. One effect for now; the slot is here so more can be
 * added without touching the loop. Indices match the PIDVD_SAVER_* enum. */
static const char *const saver_v[]  = { "OFF", "WARP STARFIELD", "DVD LOGO",
                                        "FIREWORKS" };
/* How long the picker must idle before the screensaver arms. Labels and the
 * matching real seconds stay index-aligned; the run loop reads the seconds via
 * ui_settings_saver_timeout_seconds(). */
static const char *const saver_to_v[]  = { "30 SEC", "1 MIN", "2 MIN",
                                           "5 MIN", "10 MIN" };
static const int         saver_to_s[]  = { 30, 60, 120, 300, 600 };
/* Menu composite horizontal low-pass: 0 off, 1..8 faint->strong. 4 is the
 * [1 2 1]/4 kernel; lower = sharper text / more cross-colour, higher = softer
 * text / cleaner colour. Tune to the lightest that clears the splotching. */
static const char *const filter_v[] = { "OFF", "1", "2", "3", "4",
                                        "5", "6", "7", "8" };
/* Display output / active connector: composite VEC, or HDMI (for an
 * HDMI->VGA->SCART RGB chain). Switches live in the menu. */
static const char *const output_v[] = { "COMPOSITE", "HDMI" };

static const struct {
    const char *label;
    const char *const *values;
    int n;
} rows[UI_SET_ROWS] = {
    [UI_SET_THEME]  = { "THEME",          theme_v,  7 },
    [UI_SET_LAYOUT] = { "LAYOUT",         layout_v, 4 },
    [UI_SET_ADEV]   = { "AUDIO DEVICE",   0,        0 }, /* dynamic */
    [UI_SET_VOL]    = { "VOLUME",         0,        0 }, /* dynamic */
    [UI_SET_DIM]    = { "ATTRACT DIM",    dim_v,    4 },
    [UI_SET_SAVER]  = { "SCREENSAVER",    saver_v,  4 },
    [UI_SET_SAVER_TO] = { "SAVER TIMEOUT", saver_to_v, 5 },
    [UI_SET_FILTER] = { "COMP FILTER",    filter_v, 9 },
    [UI_SET_OUTPUT] = { "OUTPUT",         output_v, 2 },
    [UI_SET_RESCAN] = { "RESCAN CATALOG", 0,        0 },
};

/* ---- tabs: a view grouping over rows[] --------------------------------- */
enum { UI_TAB_DISPLAY, UI_TAB_AUDIO, UI_TAB_IDLE, UI_TAB_SYSTEM, UI_N_TABS };

static const char *const tab_v[UI_N_TABS] = {
    "DISPLAY", "AUDIO", "IDLE", "SYSTEM"
};

/* Ordered rows per tab, -1 terminated (the order here is the on-screen order).
 * Every row must appear in exactly one tab. */
static const int tab_rows[UI_N_TABS][UI_SET_ROWS + 1] = {
    [UI_TAB_DISPLAY] = { UI_SET_THEME, UI_SET_LAYOUT, UI_SET_OUTPUT,
                         UI_SET_FILTER, -1 },
    [UI_TAB_AUDIO]   = { UI_SET_ADEV, UI_SET_VOL, -1 },
    [UI_TAB_IDLE]    = { UI_SET_DIM, UI_SET_SAVER, UI_SET_SAVER_TO, -1 },
    [UI_TAB_SYSTEM]  = { UI_SET_RESCAN, -1 },
};

int ui_settings_tab_count(void) { return UI_N_TABS; }

const char *ui_settings_tab_name(int tab)
{
    return (tab >= 0 && tab < UI_N_TABS) ? tab_v[tab] : "";
}

int ui_settings_tab_len(int tab)
{
    if (tab < 0 || tab >= UI_N_TABS)
        return 0;
    int n = 0;
    while (tab_rows[tab][n] >= 0)
        n++;
    return n;
}

int ui_settings_tab_row(int tab, int i)
{
    if (tab < 0 || tab >= UI_N_TABS || i < 0 || i >= ui_settings_tab_len(tab))
        return -1;
    return tab_rows[tab][i];
}

int ui_settings_row_tab(int row)
{
    for (int t = 0; t < UI_N_TABS; t++)
        for (int i = 0; tab_rows[t][i] >= 0; i++)
            if (tab_rows[t][i] == row)
                return t;
    return 0;
}

int ui_settings_tab_first(const ui_settings_t *s, int tab)
{
    for (int i = 0, n = ui_settings_tab_len(tab); i < n; i++)
        if (ui_settings_enabled(s, tab_rows[tab][i]))
            return tab_rows[tab][i];
    return ui_settings_tab_row(tab, 0);   /* nothing live: land on the first */
}

int ui_settings_tab_step(const ui_settings_t *s, int tab, int row, int dir)
{
    int n = ui_settings_tab_len(tab);
    if (n <= 0)
        return row;
    int pos = 0;
    for (int i = 0; i < n; i++)
        if (tab_rows[tab][i] == row) { pos = i; break; }
    for (int step = 0; step < n; step++) {
        pos = (pos + dir + n) % n;
        if (ui_settings_enabled(s, tab_rows[tab][pos]))
            return tab_rows[tab][pos];
    }
    return row;   /* none live: stay put */
}

static int *field(ui_settings_t *s, int row)
{
    switch (row) {
    case UI_SET_THEME:  return &s->theme;
    case UI_SET_LAYOUT: return &s->layout;
    case UI_SET_DIM:    return &s->attract_dim;
    case UI_SET_SAVER:  return &s->saver;
    case UI_SET_SAVER_TO: return &s->saver_to;
    case UI_SET_FILTER: return &s->comp_filter;
    case UI_SET_OUTPUT: return &s->output;
    default:            return 0;   /* ADEV/VOL/RESCAN handled specially */
    }
}

const char *ui_settings_label(int row)
{
    return (row >= 0 && row < UI_SET_ROWS) ? rows[row].label : "";
}

/* A row can be inert in the current context: COMP FILTER band-limits luma below
 * the composite colour subcarrier, so on HDMI/RGB it does nothing and is shown
 * dimmed + unselectable. Returns 1 if the row is live, 0 if disabled. */
int ui_settings_enabled(const ui_settings_t *s, int row)
{
    if (row == UI_SET_FILTER)
        return s->output == 0;   /* composite (VEC) only */
    if (row == UI_SET_SAVER_TO)
        return s->saver != 0;    /* meaningless when SCREENSAVER is OFF */
    return 1;
}

int ui_settings_saver_timeout_seconds(const ui_settings_t *s)
{
    int n = (int)(sizeof saver_to_s / sizeof saver_to_s[0]);
    int i = (s->saver_to >= 0 && s->saver_to < n) ? s->saver_to : 0;
    return saver_to_s[i];
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
        return s->dev_label[i];   /* full name; render_settings windows/scrolls it */
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

int ui_settings_get(const ui_settings_t *s, int row)
{
    if (row == UI_SET_ADEV) return s->dev_sel;
    if (row == UI_SET_VOL)  return s->volume;
    const int *f = field((ui_settings_t *)s, row);
    return f ? *f : 0;
}

void ui_settings_set(ui_settings_t *s, int row, int val)
{
    if (row == UI_SET_ADEV) {
        if (s->n_dev > 0) {
            if (val < 0) val = 0;
            if (val >= s->n_dev) val = s->n_dev - 1;
            s->dev_sel = val;
            snprintf(s->audio_dev, sizeof(s->audio_dev), "%s", s->dev_id[val]);
        }
        return;
    }
    if (row == UI_SET_VOL) { s->volume = val; return; }
    int *f = field(s, row);
    if (f) *f = val;
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
    s->saver_to = 2;       /* arm after 2 min idle (index into saver_to_v) */
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
            if (!strcmp(line, "theme"))        s->theme = atoi(v) % 7;
            else if (!strcmp(line, "layout"))  s->layout = atoi(v) % 4;
            else if (!strcmp(line, "volume")) {
                s->volume = atoi(v);
                if (s->volume < 0) s->volume = 0;
                if (s->volume > 100) s->volume = 100;
            }
            else if (!strcmp(line, "adev"))
                snprintf(s->audio_dev, sizeof(s->audio_dev), "%s", v);
            else if (!strcmp(line, "dim"))     s->attract_dim = atoi(v) & 3;
            else if (!strcmp(line, "saver"))   s->saver = atoi(v) % 4;
            else if (!strcmp(line, "saver_to")) s->saver_to = atoi(v) % 5;
            else if (!strcmp(line, "filter")) {
                s->comp_filter = atoi(v);
                if (s->comp_filter < 0 || s->comp_filter > 8)
                    s->comp_filter = 5;
            }
            else if (!strcmp(line, "output")) s->output = atoi(v) & 1;
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
                   "saver=%d\nsaver_to=%d\nfilter=%d\noutput=%d\nlast_std=%d\n"
                   "last_disc=%s\nlast_title=%d\nlast_sector=%d\nlast_seconds=%d\n",
                s->theme, s->layout, s->volume, s->audio_dev,
                s->attract_dim, s->saver, s->saver_to, s->comp_filter, s->output,
                s->last_standard, s->last_disc,
                s->last_title, s->last_sector, s->last_seconds);
        fclose(f);
    }
    if (mnt) {
        (void)system("sync");
        (void)system("umount " CFG_MNT " 2>/dev/null || true");
    }
}
