/* Catalog: the browsable model of a source (one mounted root). Listing
 * is instant (readdir); IFO metadata arrives from a background scan
 * thread and is cached in <root>/.pidvd/catalog so a re-plugged drive
 * is hot in one read. docs/UI.md §7. */
#ifndef PIDVD_UI_CATALOG_H
#define PIDVD_UI_CATALOG_H

#include "ui/render.h"

typedef struct {
    ui_item_t v;
    char relpath[512];   /* relative to root, "" for ".." */
} cat_entry_t;

typedef struct catalog catalog_t;

catalog_t *catalog_open(const char *root);
void catalog_close(catalog_t *cat);

/* Enter a directory (entry must be a dir) / go to parent. */
void catalog_enter(catalog_t *cat, int idx);
void catalog_up(catalog_t *cat);
bool catalog_at_root(const catalog_t *cat);
const char *catalog_cwd(const catalog_t *cat);   /* "" at root */

/* Drop all cached metadata and rescan from scratch. */
void catalog_rescan(catalog_t *cat);

/* Total ISOs directly under root tree level (autoplay rule helper):
 * returns count and writes the sole ISO's path if exactly one. */
int catalog_root_iso_count(catalog_t *cat, char *only, int cap);

/* Total .iso files in the whole library (recursive walk of the root tree),
 * for the header's "n DISCS" count. Computed once at open / on rescan. */
int catalog_iso_total(const catalog_t *cat);

/* Snapshot access for rendering. Hold the lock only while drawing. */
void catalog_lock(catalog_t *cat);
void catalog_unlock(catalog_t *cat);
int catalog_count(const catalog_t *cat);             /* under lock */
const cat_entry_t *catalog_get(const catalog_t *cat, int idx);
bool catalog_scanning(const catalog_t *cat);

/* Absolute path of an entry (root + relpath). */
void catalog_abspath(const catalog_t *cat, const cat_entry_t *e,
                     char *out, int cap);

#endif
