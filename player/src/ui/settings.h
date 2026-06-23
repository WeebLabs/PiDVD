/* Picker settings: theme, layout, picker video mode, audio out, attract
 * dim — plus appliance state (last disc/standard for the shelf and the
 * picker's startup mode). Persisted as key=value in pidvd.cfg on the SD
 * boot partition (the appliance's NVRAM); PIDVD_CFG overrides the path
 * for host development. docs/UI.md §5.6. */
#ifndef PIDVD_UI_SETTINGS_H
#define PIDVD_UI_SETTINGS_H

enum { UI_SET_THEME, UI_SET_LAYOUT, UI_SET_ADEV, UI_SET_VOL,
       UI_SET_DIM, UI_SET_SAVER, UI_SET_FILTER, UI_SET_RESCAN, UI_SET_ROWS };

/* AUTO plus up to a handful of real output cards. */
#define UI_AUDIO_DEV_MAX 6

typedef struct {
    int theme;        /* index into pidvd_themes */
    int layout;       /* 0 console, 1 marquee, 2 ledger */
    int volume;       /* 0..100, applied to the output card's mixer (UAC) */
    int attract_dim;  /* 0 off, 1 5min, 2 15min, 3 30min */
    int saver;        /* screensaver: 0 off, 1 warp starfield (PIDVD_SAVER_*) */
    int comp_filter;  /* 0 off..8: menu composite horizontal low-pass strength */
    char audio_dev[20]; /* selected output card id; "" = AUTO (USB->PWM) */
    /* The menu is always 240p NTSC; the disc's own standard drives
     * playback. last_* are kept for the NOW PLAYING resume shelf. */
    char last_disc[512];
    int last_standard; /* pidvd_standard_t of the last played disc */

    /* Runtime audio-device list, filled by the platform each time the menu
     * opens (not persisted). Index 0 is always AUTO; 1.. are real cards. */
    int  n_dev;
    int  dev_sel;                       /* index into dev_* of audio_dev */
    char dev_id[UI_AUDIO_DEV_MAX][20];  /* "" for AUTO, else ALSA card id */
    char dev_label[UI_AUDIO_DEV_MAX][28];
} ui_settings_t;

void ui_settings_load(ui_settings_t *s);
void ui_settings_save(const ui_settings_t *s);

/* Row label / current value as displayed text. */
const char *ui_settings_label(int row);
const char *ui_settings_value(const ui_settings_t *s, int row);
/* Cycle a row's value by dir (+1/-1). Returns 1 if it changed. */
int ui_settings_cycle(ui_settings_t *s, int row, int dir);

/* Point dev_sel at the entry whose id matches audio_dev (else AUTO). Call
 * after the platform fills the dev_* list. */
void ui_settings_resolve_dev(ui_settings_t *s);

#endif
