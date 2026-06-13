/* Catalog: instant listing + background IFO scanning + sidecar cache.
 * The scan thread opens one ISO at a time with the core disc model and
 * fills metadata under the lock; the UI redraws every field anyway, so
 * results just appear. Cache lines are TSV keyed by relpath|size|mtime —
 * a re-plugged unchanged drive never reopens an ISO. */
#include "ui/catalog.h"

#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "core/disc.h"

#define MAX_ENTRIES 2048

struct catalog {
    char root[512];
    char cwd[512];               /* relative, "" at root */
    cat_entry_t *ents;
    int n;
    pthread_t thr;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool stop;
    bool scanning;
    int generation;              /* bumped on chdir/rescan */
    FILE *cache_append;
};

/* ---- cache ------------------------------------------------------------ */

static void cache_path(const catalog_t *c, char *out, int cap)
{
    snprintf(out, (size_t)cap, "%s/.pidvd/catalog", c->root);
}

static void cache_write(catalog_t *c, const cat_entry_t *e,
                        uint64_t size, long mtime)
{
    if (!c->cache_append) {
        char dir[600], path[600];
        snprintf(dir, sizeof(dir), "%s/.pidvd", c->root);
        mkdir(dir, 0755);
        cache_path(c, path, sizeof(path));
        c->cache_append = fopen(path, "a");
        if (!c->cache_append)
            return;
    }
    const ui_item_t *v = &e->v;
    fprintf(c->cache_append,
            "%s\t%llu\t%ld\t%d\t%s\t%d\t%d\t%d\t%d\t%d\t%u\t%d\t%d\t%.1f\t",
            e->relpath, (unsigned long long)size, mtime,
            v->scan_failed ? 1 : 0, v->volid[0] ? v->volid : "-",
            v->standard, v->width, v->height, v->wide ? 1 : 0,
            v->letterboxed ? 1 : 0, v->region_mask, v->titles, v->chapters,
            v->longest);
    for (int a = 0; a < v->n_audio; a++)
        fprintf(c->cache_append, "%s%s", a ? "," : "", v->audio[a]);
    fprintf(c->cache_append, "\t%s\n", v->subs[0] ? v->subs : "-");
    fflush(c->cache_append);
}

/* Look the entry up in the cache file; returns true on hit. */
static bool cache_lookup(catalog_t *c, cat_entry_t *e, uint64_t size,
                         long mtime)
{
    char path[600];
    cache_path(c, path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    char line[1024];
    bool hit = false;
    while (fgets(line, sizeof(line), f)) {
        char *save = NULL;
        char *fields[16];
        int nf = 0;
        for (char *t = strtok_r(line, "\t\n", &save);
             t && nf < 16; t = strtok_r(NULL, "\t\n", &save))
            fields[nf++] = t;
        if (nf < 14)
            continue;
        if (strcmp(fields[0], e->relpath))
            continue;
        if (strtoull(fields[1], NULL, 10) != size
            || strtol(fields[2], NULL, 10) != mtime)
            continue;
        ui_item_t *v = &e->v;
        v->scan_failed = atoi(fields[3]) != 0;
        if (strcmp(fields[4], "-"))
            snprintf(v->volid, sizeof(v->volid), "%s", fields[4]);
        v->standard = atoi(fields[5]);
        v->width = atoi(fields[6]);
        v->height = atoi(fields[7]);
        v->wide = atoi(fields[8]) != 0;
        v->letterboxed = atoi(fields[9]) != 0;
        v->region_mask = (uint8_t)atoi(fields[10]);
        v->titles = atoi(fields[11]);
        v->chapters = atoi(fields[12]);
        v->longest = atof(fields[13]);
        v->n_audio = 0;
        if (nf > 14 && strcmp(fields[14], "-")) {
            char *as = NULL;
            for (char *t = strtok_r(fields[14], ",", &as);
                 t && v->n_audio < 4; t = strtok_r(NULL, ",", &as))
                snprintf(v->audio[v->n_audio++], sizeof(v->audio[0]),
                         "%s", t);
        }
        if (nf > 15 && strcmp(fields[15], "-"))
            snprintf(v->subs, sizeof(v->subs), "%s", fields[15]);
        v->scanned = true;
        hit = true;
        /* keep going: later lines overwrite earlier (rescans append) */
    }
    fclose(f);
    return hit;
}

/* ---- listing ----------------------------------------------------------- */

static bool is_iso(const char *name)
{
    size_t n = strlen(name);
    return n > 4 && !strcasecmp(name + n - 4, ".iso");
}

static void display_name(const char *file, char *out, int cap)
{
    snprintf(out, (size_t)cap, "%s", file);
    size_t n = strlen(out);
    if (n > 4 && !strcasecmp(out + n - 4, ".iso"))
        out[n - 4] = 0;
    for (char *p = out; *p; p++)
        if (*p == '_')
            *p = ' ';
}

static int ent_cmp(const void *pa, const void *pb)
{
    const cat_entry_t *a = pa, *b = pb;
    if (a->v.is_parent != b->v.is_parent)
        return a->v.is_parent ? -1 : 1;
    if (a->v.is_dir != b->v.is_dir)
        return a->v.is_dir ? -1 : 1;
    return strcasecmp(a->v.name, b->v.name);
}

/* count children / sum ISO sizes one level deep (cheap stat pass) */
static void dir_quickstats(const char *abs, ui_item_t *v)
{
    DIR *d = opendir(abs);
    if (!d)
        return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.')
            continue;
        char p[1024];
        snprintf(p, sizeof(p), "%s/%s", abs, de->d_name);
        struct stat st;
        if (stat(p, &st))
            continue;
        if (S_ISDIR(st.st_mode)) {
            v->n_items++;
        } else if (is_iso(de->d_name)) {
            v->n_items++;
            v->size += (uint64_t)st.st_size;
        }
    }
    closedir(d);
}

/* rebuild ents for cwd; call with lock held */
static void list_cwd(catalog_t *c)
{
    c->n = 0;
    char abs[1100];
    snprintf(abs, sizeof(abs), "%s%s%s", c->root, c->cwd[0] ? "/" : "",
             c->cwd);

    if (c->cwd[0]) {
        cat_entry_t *e = &c->ents[c->n++];
        memset(e, 0, sizeof(*e));
        e->v.is_dir = e->v.is_parent = true;
        snprintf(e->v.name, sizeof(e->v.name), "..");
    }

    DIR *d = opendir(abs);
    if (!d)
        return;
    struct dirent *de;
    while ((de = readdir(d)) && c->n < MAX_ENTRIES) {
        if (de->d_name[0] == '.')
            continue;
        char p[1600];
        snprintf(p, sizeof(p), "%s/%s", abs, de->d_name);
        struct stat st;
        if (stat(p, &st))
            continue;
        bool dir = S_ISDIR(st.st_mode);
        if (!dir && !is_iso(de->d_name))
            continue;
        cat_entry_t *e = &c->ents[c->n++];
        memset(e, 0, sizeof(*e));
        e->v.is_dir = dir;
        snprintf(e->relpath, sizeof(e->relpath), "%s%s%s", c->cwd,
                 c->cwd[0] ? "/" : "", de->d_name);
        if (dir) {
            snprintf(e->v.name, sizeof(e->v.name), "%s", de->d_name);
            dir_quickstats(p, &e->v);
        } else {
            display_name(de->d_name, e->v.name, sizeof(e->v.name));
            e->v.size = (uint64_t)st.st_size;
            if (cache_lookup(c, e, e->v.size, (long)st.st_mtime))
                e->v.scanned = true;
        }
    }
    closedir(d);
    qsort(c->ents, (size_t)c->n, sizeof(c->ents[0]), ent_cmp);
    pthread_cond_signal(&c->cv);
}

/* ---- scan thread ------------------------------------------------------- */

static void scan_one(catalog_t *c, int gen, const char *relpath)
{
    char abs[1100];
    snprintf(abs, sizeof(abs), "%s/%s", c->root, relpath);

    /* slow part outside the lock */
    ui_item_t m;
    memset(&m, 0, sizeof(m));
    pidvd_disc_t *d = pidvd_disc_open(abs);
    if (!d) {
        m.scan_failed = true;
    } else {
        snprintf(m.volid, sizeof(m.volid), "%s", pidvd_disc_volume_id(d));
        m.region_mask = pidvd_disc_region_mask(d);
        m.titles = pidvd_disc_title_count(d);
        const pidvd_title_t *longest = NULL;
        for (int i = 0; i < m.titles; i++) {
            const pidvd_title_t *t = pidvd_disc_title(d, i);
            m.chapters += t->chapters;
            if (!longest || t->seconds > longest->seconds)
                longest = t;
        }
        if (longest) {
            m.standard = longest->standard;
            m.width = longest->width;
            m.height = longest->height;
            m.wide = longest->aspect == PIDVD_ASPECT_16_9;
            m.letterboxed = longest->letterboxed;
            m.longest = longest->seconds;
            for (int a = 0; a < longest->n_audio && m.n_audio < 4; a++) {
                const pidvd_audio_stream_t *as = &longest->audio[a];
                const char *layout = as->channels >= 6 ? "5.1"
                                   : as->channels >= 3 ? "SURR" : "2.0";
                char lang[4];
                snprintf(lang, sizeof(lang), "%s", as->lang);
                for (char *p = lang; *p; p++)
                    *p = (char)((*p >= 'a' && *p <= 'z') ? *p - 32 : *p);
                snprintf(m.audio[m.n_audio++], sizeof(m.audio[0]),
                         "%s %s %s", as->format, layout, lang);
            }
            int sl = 0;
            for (int s = 0; s < longest->n_subp
                            && sl < (int)sizeof(m.subs) - 4; s++) {
                char lang[4];
                snprintf(lang, sizeof(lang), "%s", longest->subp[s].lang);
                for (char *p = lang; *p; p++)
                    *p = (char)((*p >= 'a' && *p <= 'z') ? *p - 32 : *p);
                sl += snprintf(m.subs + sl, sizeof(m.subs) - (size_t)sl,
                               "%s%s", sl ? " " : "", lang);
            }
        }
        pidvd_disc_close(d);
    }
    m.scanned = true;

    struct stat st;
    long mtime = stat(abs, &st) ? 0 : (long)st.st_mtime;

    pthread_mutex_lock(&c->mu);
    if (gen == c->generation) {
        for (int i = 0; i < c->n; i++) {
            cat_entry_t *e = &c->ents[i];
            if (!e->v.is_dir && !strcmp(e->relpath, relpath)) {
                /* keep listing identity, take scanned metadata */
                ui_item_t keep = e->v;
                e->v = m;
                memcpy(e->v.name, keep.name, sizeof(e->v.name));
                e->v.is_dir = false;
                e->v.is_parent = false;
                e->v.size = keep.size;
                cache_write(c, e, keep.size, mtime);
                break;
            }
        }
    }
    pthread_mutex_unlock(&c->mu);
}

static void *scan_main(void *arg)
{
    catalog_t *c = arg;
    pthread_mutex_lock(&c->mu);
    for (;;) {
        if (c->stop)
            break;
        int idx = -1;
        for (int i = 0; i < c->n; i++)
            if (!c->ents[i].v.is_dir && !c->ents[i].v.scanned) {
                idx = i;
                break;
            }
        if (idx < 0) {
            c->scanning = false;
            pthread_cond_wait(&c->cv, &c->mu);
            continue;
        }
        c->scanning = true;
        char rel[512];
        snprintf(rel, sizeof(rel), "%s", c->ents[idx].relpath);
        int gen = c->generation;
        pthread_mutex_unlock(&c->mu);
        scan_one(c, gen, rel);
        pthread_mutex_lock(&c->mu);
    }
    pthread_mutex_unlock(&c->mu);
    return NULL;
}

/* ---- API ---------------------------------------------------------------- */

catalog_t *catalog_open(const char *root)
{
    catalog_t *c = calloc(1, sizeof(*c));
    snprintf(c->root, sizeof(c->root), "%s", root);
    c->ents = calloc(MAX_ENTRIES, sizeof(*c->ents));
    pthread_mutex_init(&c->mu, NULL);
    pthread_cond_init(&c->cv, NULL);
    pthread_mutex_lock(&c->mu);
    list_cwd(c);
    pthread_mutex_unlock(&c->mu);
    pthread_create(&c->thr, NULL, scan_main, c);
    return c;
}

void catalog_close(catalog_t *c)
{
    if (!c)
        return;
    pthread_mutex_lock(&c->mu);
    c->stop = true;
    pthread_cond_signal(&c->cv);
    pthread_mutex_unlock(&c->mu);
    pthread_join(c->thr, NULL);
    if (c->cache_append)
        fclose(c->cache_append);
    free(c->ents);
    free(c);
}

void catalog_enter(catalog_t *c, int idx)
{
    pthread_mutex_lock(&c->mu);
    if (idx >= 0 && idx < c->n && c->ents[idx].v.is_dir
        && !c->ents[idx].v.is_parent) {
        snprintf(c->cwd, sizeof(c->cwd), "%s", c->ents[idx].relpath);
        c->generation++;
        list_cwd(c);
    }
    pthread_mutex_unlock(&c->mu);
}

void catalog_up(catalog_t *c)
{
    pthread_mutex_lock(&c->mu);
    char *slash = strrchr(c->cwd, '/');
    if (slash)
        *slash = 0;
    else
        c->cwd[0] = 0;
    c->generation++;
    list_cwd(c);
    pthread_mutex_unlock(&c->mu);
}

bool catalog_at_root(const catalog_t *c) { return c->cwd[0] == 0; }
const char *catalog_cwd(const catalog_t *c) { return c->cwd; }

void catalog_rescan(catalog_t *c)
{
    pthread_mutex_lock(&c->mu);
    char path[600];
    cache_path(c, path, sizeof(path));
    if (c->cache_append) {
        fclose(c->cache_append);
        c->cache_append = NULL;
    }
    remove(path);
    c->generation++;
    list_cwd(c);
    for (int i = 0; i < c->n; i++)
        if (!c->ents[i].v.is_dir)
            c->ents[i].v.scanned = false;
    pthread_cond_signal(&c->cv);
    pthread_mutex_unlock(&c->mu);
}

int catalog_root_iso_count(catalog_t *c, char *only, int cap)
{
    pthread_mutex_lock(&c->mu);
    int count = 0;
    for (int i = 0; i < c->n; i++)
        if (!c->ents[i].v.is_dir) {
            if (count == 0 && only)
                catalog_abspath(c, &c->ents[i], only, cap);
            count++;
        } else if (!c->ents[i].v.is_parent) {
            count += 2; /* a directory disables the autoplay rule */
        }
    pthread_mutex_unlock(&c->mu);
    return count;
}

void catalog_lock(catalog_t *c)   { pthread_mutex_lock(&c->mu); }
void catalog_unlock(catalog_t *c) { pthread_mutex_unlock(&c->mu); }
int catalog_count(const catalog_t *c) { return c->n; }
const cat_entry_t *catalog_get(const catalog_t *c, int idx)
{
    return (idx >= 0 && idx < c->n) ? &c->ents[idx] : NULL;
}
bool catalog_scanning(const catalog_t *c) { return c->scanning; }

void catalog_abspath(const catalog_t *c, const cat_entry_t *e,
                     char *out, int cap)
{
    snprintf(out, (size_t)cap, "%s/%s", c->root, e->relpath);
}
