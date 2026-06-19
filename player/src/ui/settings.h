/* Picker settings: theme, layout, picker video mode, audio out, attract
 * dim — plus appliance state (last disc/standard for the shelf and the
 * picker's startup mode). Persisted as key=value in pidvd.cfg on the SD
 * boot partition (the appliance's NVRAM); PIDVD_CFG overrides the path
 * for host development. docs/UI.md §5.6. */
#ifndef PIDVD_UI_SETTINGS_H
#define PIDVD_UI_SETTINGS_H

enum { UI_SET_THEME, UI_SET_LAYOUT, UI_SET_AUDIO,
       UI_SET_DIM, UI_SET_FILTER, UI_SET_RESCAN, UI_SET_ROWS };

typedef struct {
    int theme;        /* index into pidvd_themes */
    int layout;       /* 0 console, 1 marquee, 2 ledger */
    int audio_out;    /* reserved; display-synchronized PCM is mandatory */
    int attract_dim;  /* 0 off, 1 5min, 2 15min, 3 30min */
    int comp_filter;  /* 0 off..8: menu composite horizontal low-pass strength */
    /* The menu is always 240p NTSC; the disc's own standard drives
     * playback. last_* are kept for the NOW PLAYING resume shelf. */
    char last_disc[512];
    int last_standard; /* pidvd_standard_t of the last played disc */
} ui_settings_t;

void ui_settings_load(ui_settings_t *s);
void ui_settings_save(const ui_settings_t *s);

/* Row label / current value as displayed text. */
const char *ui_settings_label(int row);
const char *ui_settings_value(const ui_settings_t *s, int row);
/* Cycle a row's value by dir (+1/-1). Returns 1 if it changed. */
int ui_settings_cycle(ui_settings_t *s, int row, int dir);

#endif
