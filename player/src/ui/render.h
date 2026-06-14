/* Pure picker rendering: a view model in, pixels out. No platform, no
 * threads, no clock — the run loop (picker.c) and the host preview tool
 * (tools/uipreview.c) both drive this, so every screen can be verified
 * on a Mac before it ever reaches the CRT. docs/UI.md §5. */
#ifndef PIDVD_UI_RENDER_H
#define PIDVD_UI_RENDER_H

#include <stdbool.h>
#include <stdint.h>

#include "ui/draw.h"
#include "ui/settings.h"

typedef struct {
    uint32_t bg, panel, dim, text, bright, hot, bar, bartxt;
} ui_theme_t;

#define PIDVD_N_THEMES 5
extern const ui_theme_t pidvd_themes[PIDVD_N_THEMES];

/* One browsable thing: a directory or a DVD ISO (+ metadata once the
 * catalog scan has reached it). */
typedef struct {
    char name[96];        /* display name: filename sans .iso, _ -> space */
    bool is_dir;
    bool is_parent;       /* the ".." entry */
    uint64_t size;        /* bytes; for dirs, total of contained ISOs */
    int  n_items;         /* dirs: direct children */
    bool scanned;
    bool scan_failed;     /* not a DVD image / unreadable */
    /* IFO facts (valid when scanned && !scan_failed) */
    char volid[33];
    int  standard;        /* pidvd_standard_t */
    int  width, height;
    bool wide, letterboxed;
    uint8_t region_mask;
    int  titles, chapters;
    double longest;       /* seconds */
    char audio[4][24];    /* "AC-3 5.1 EN" */
    int  n_audio;
    char subs[64];        /* "EN DE FR NL" */
} ui_item_t;

typedef enum { UI_ATTRACT, UI_BROWSE, UI_SETTINGS } ui_screen_t;
typedef enum { UI_CONSOLE, UI_MARQUEE, UI_LEDGER, UI_WIREFRAME } ui_layout_t;

typedef struct {
    ui_screen_t screen;
    const ui_settings_t *set;     /* theme/layout read from here */

    /* BROWSE */
    const ui_item_t *const *items;
    int n_items;
    int sel;                      /* selected index */
    int scroll;                   /* first visible index */
    bool at_root;
    const char *source;           /* "USB" */
    const char *path;             /* "/Action" ("" at root) */
    const char *now_playing;      /* display name, NULL if none */
    bool scanning;                /* catalog thread busy */

    /* ATTRACT */
    const char *notice;           /* e.g. "NO DVD ISOS FOUND" */

    /* SETTINGS */
    int set_sel;

    int tick;                     /* field-rate frame counter (animation) */
} ui_view_t;

void pidvd_ui_render(ui_canvas_t *c, const ui_view_t *v);

/* How many list rows fit for the given layout/canvas — the run loop
 * needs this to keep sel/scroll consistent with what's drawn. */
int pidvd_ui_visible_rows(const ui_view_t *v, int canvas_h);

#endif
