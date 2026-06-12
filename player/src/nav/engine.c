#include "nav/engine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>

#include <dvdnav/dvdnav.h>

#include "core/disc.h"
#include "decode/spu.h"
#include "decode/video_mpeg2.h"
#include "demux/ps.h"
#include "platform/platform.h"
#include "present/presenter.h"

#define FRAME_W 720
#define FRAME_H 576

struct engine {
    dvdnav_t *nav;
    pidvd_video_t *video;
    pidvd_presenter_t *pres;
    pidvd_vdec_t *vdec;
    pidvd_spu_t *spu;
    pidvd_input_t *input;
    pidvd_ps_t ps;

    pidvd_standard_t std;
    /* last decoded frame (planar), kept for re-pushing during stills */
    uint8_t *fy, *fu, *fv;
    int vw, vh;
    bool have_frame;
    pthread_mutex_t spu_lock;   /* spu state vs presenter blend thread */
    bool tff, rff;
    long frames;
    /* producer timing */
    double t_prev, t_acc, t_max;
    long t_n;
};

/* ---- compositing (presenter thread) ----------------------------------- */

static void blend_overlay(void *opaque, uint8_t *rgb, int w, int h)
{
    struct engine *e = opaque;
    pidvd_overlay_t ovl;
    pthread_mutex_lock(&e->spu_lock);
    if (pidvd_spu_overlay(e->spu, &ovl)) {
        /* clip once; transparent skipped, opaque copied, the rare
         * translucent pixels blended without division */
        int x0 = ovl.x < 0 ? -ovl.x : 0;
        int y0 = ovl.y < 0 ? -ovl.y : 0;
        int x1 = ovl.x + ovl.w > w ? w - ovl.x : ovl.w;
        int y1 = ovl.y + ovl.h > h ? h - ovl.y : ovl.h;
        for (int y = y0; y < y1; y++) {
            const uint8_t *p = &ovl.rgba[((size_t)y * ovl.w + x0) * 4];
            uint8_t *d = &rgb[(((size_t)(ovl.y + y) * w)
                               + ovl.x + x0) * 4];
            for (int x = x0; x < x1; x++, p += 4, d += 4) {
                uint8_t a = p[3];
                if (a == 0)
                    continue;
                if (a == 255) {
                    d[0] = p[0]; d[1] = p[1]; d[2] = p[2];
                    continue;
                }
                unsigned ia = 255 - a;
                unsigned t;
                t = p[0] * a + d[0] * ia + 128; d[0] = (t + (t >> 8)) >> 8;
                t = p[1] * a + d[1] * ia + 128; d[1] = (t + (t >> 8)) >> 8;
                t = p[2] * a + d[2] * ia + 128; d[2] = (t + (t >> 8)) >> 8;
            }
        }
    }
    pthread_mutex_unlock(&e->spu_lock);
}

static void push_composed(struct engine *e)
{
    if (!e->have_frame)
        return;
    pidvd_presenter_push(e->pres, e->fy, e->fu, e->fv, e->vw, e->vh,
                         e->tff, e->rff);
    e->frames++;
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void on_frame(void *opaque, const uint8_t *y, const uint8_t *u,
                     const uint8_t *v, int w, int h, bool tff, bool rff)
{
    struct engine *e = opaque;
    double t = now_s();
    if (e->t_prev > 0) {
        double dt = t - e->t_prev;
        e->t_acc += dt;
        if (dt > e->t_max)
            e->t_max = dt;
        if (++e->t_n == 100) {
            fprintf(stderr, "decode: avg %.1fms max %.1fms per frame\n",
                    e->t_acc / e->t_n * 1000.0, e->t_max * 1000.0);
            e->t_acc = e->t_max = 0;
            e->t_n = 0;
        }
    }
    e->t_prev = t;
    if (w > FRAME_W) w = FRAME_W;
    if (h > FRAME_H) h = FRAME_H;
    memcpy(e->fy, y, (size_t)w * h);
    memcpy(e->fu, u, (size_t)w * h / 4);
    memcpy(e->fv, v, (size_t)w * h / 4);
    e->vw = w;
    e->vh = h;
    e->tff = tff;
    e->rff = rff;
    e->have_frame = true;
    push_composed(e);
}

static void on_video_es(void *opaque, const uint8_t *data, size_t len)
{
    struct engine *e = opaque;
    pidvd_vdec_push(e->vdec, data, len);
}

static void on_spu_es(void *opaque, int substream, const uint8_t *data,
                      size_t len)
{
    struct engine *e = opaque;
    pthread_mutex_lock(&e->spu_lock);
    pidvd_spu_packet(e->spu, substream, data, len);
    pthread_mutex_unlock(&e->spu_lock);
}

/* ---- highlight ------------------------------------------------------- */

static void update_highlight(struct engine *e, bool repush)
{
    pci_t *pci = dvdnav_get_current_nav_pci(e->nav);
    int32_t button = 0;
    dvdnav_get_current_highlight(e->nav, &button);
    bool changed;
    pthread_mutex_lock(&e->spu_lock);
    if (pci && button > 0 && pci->hli.hl_gi.btn_ns > 0) {
        dvdnav_highlight_area_t hl;
        if (dvdnav_get_highlight_area(pci, button, 0, &hl) == DVDNAV_STATUS_OK)
            changed = pidvd_spu_set_highlight(e->spu, hl.sx, hl.sy,
                                              hl.ex, hl.ey, hl.palette);
        else
            changed = pidvd_spu_clear_highlight(e->spu);
    } else {
        changed = pidvd_spu_clear_highlight(e->spu);
    }
    pthread_mutex_unlock(&e->spu_lock);
    if (changed && repush)
        push_composed(e);
}

/* ---- input ------------------------------------------------------------ */

static bool handle_key(struct engine *e, pidvd_key_t k)
{
    pci_t *pci = dvdnav_get_current_nav_pci(e->nav);
    switch (k) {
    case PIDVD_KEY_UP:     dvdnav_upper_button_select(e->nav, pci); break;
    case PIDVD_KEY_DOWN:   dvdnav_lower_button_select(e->nav, pci); break;
    case PIDVD_KEY_LEFT:   dvdnav_left_button_select(e->nav, pci);  break;
    case PIDVD_KEY_RIGHT:  dvdnav_right_button_select(e->nav, pci); break;
    case PIDVD_KEY_ENTER:  dvdnav_button_activate(e->nav, pci);     break;
    case PIDVD_KEY_MENU:   dvdnav_menu_call(e->nav, DVD_MENU_Escape);
                           dvdnav_menu_call(e->nav, DVD_MENU_Root); break;
    case PIDVD_KEY_TITLE:  dvdnav_menu_call(e->nav, DVD_MENU_Title); break;
    case PIDVD_KEY_NEXT_CHAPTER: dvdnav_next_pg_search(e->nav);     break;
    case PIDVD_KEY_PREV_CHAPTER: dvdnav_prev_pg_search(e->nav);     break;
    case PIDVD_KEY_STOP:   return false;
    default: break;
    }
    if (k == PIDVD_KEY_UP || k == PIDVD_KEY_DOWN || k == PIDVD_KEY_LEFT
        || k == PIDVD_KEY_RIGHT)
        update_highlight(e, true);
    return true;
}

/* ---- engine ------------------------------------------------------------ */

int pidvd_nav_play(const char *iso_path)
{
    struct engine e = { 0 };
    uint8_t mem[DVD_VIDEO_LB_LEN];

    if (dvdnav_open(&e.nav, iso_path) != DVDNAV_STATUS_OK) {
        fprintf(stderr, "nav: cannot open %s\n", iso_path);
        return 1;
    }
    dvdnav_set_readahead_flag(e.nav, 1);
    dvdnav_set_PGC_positioning_flag(e.nav, 1);

    e.std = PIDVD_STD_PAL;   /* corrected on first VTS change */
    e.video = pidvd_video_open(e.std);
    if (!e.video)
        return 1;
    pthread_mutex_init(&e.spu_lock, NULL);
    pidvd_frame_t *dims = pidvd_video_begin_frame(e.video);
    e.pres = pidvd_presenter_start(e.video, dims->width, dims->height,
                                   blend_overlay, &e);
    e.vdec = pidvd_vdec_new(on_frame, &e);
    e.spu = pidvd_spu_new();
    e.input = pidvd_input_open();
    e.fy = calloc(1, FRAME_W * FRAME_H);
    e.fu = calloc(1, FRAME_W * FRAME_H / 4);
    e.fv = calloc(1, FRAME_W * FRAME_H / 4);
    pidvd_ps_init(&e.ps, on_video_es, &e);
    e.ps.spu_cb = on_spu_es;

    bool running = true;
    unsigned tick = 0;
    while (running) {
        if ((tick++ & 63) == 0) {
            pidvd_key_t k = pidvd_input_poll(e.input);
            if (k != PIDVD_KEY_NONE && !handle_key(&e, k))
                break;
        }

        /* cache blocks avoid a per-sector memcpy out of dvdnav */
        uint8_t *blk = mem;
        int32_t event = 0, len = 0;
        if (dvdnav_get_next_cache_block(e.nav, &blk, &event, &len)
            != DVDNAV_STATUS_OK) {
            fprintf(stderr, "nav: %s\n", dvdnav_err_to_string(e.nav));
            break;
        }

        switch (event) {
        case DVDNAV_BLOCK_OK:
            pidvd_ps_push_sector(&e.ps, blk);
            break;
        case DVDNAV_NOP:
            break;
        case DVDNAV_STILL_FRAME: {
            dvdnav_still_event_t *st = (dvdnav_still_event_t *)blk;
            /* show the still, poll input at 50Hz; honor finite timers */
            int remain_ms = (st->length == 0xff) ? -1 : st->length * 1000;
            pidvd_key_t k = PIDVD_KEY_NONE;
            while (running) {
                k = pidvd_input_poll(e.input);
                if (k != PIDVD_KEY_NONE) {
                    if (!handle_key(&e, k))
                        running = false;
                    break;   /* any action ends our wait; VM decides */
                }
                usleep(20000);
                if (remain_ms > 0 && (remain_ms -= 20) <= 0) {
                    dvdnav_still_skip(e.nav);
                    break;
                }
            }
            if (k == PIDVD_KEY_ENTER)
                dvdnav_still_skip(e.nav);
            break;
        }
        case DVDNAV_WAIT:
            dvdnav_wait_skip(e.nav);
            break;
        case DVDNAV_SPU_CLUT_CHANGE:
            pthread_mutex_lock(&e.spu_lock);
            pidvd_spu_set_clut(e.spu, (const uint32_t *)blk);
            pthread_mutex_unlock(&e.spu_lock);
            break;
        case DVDNAV_SPU_STREAM_CHANGE: {
            dvdnav_spu_stream_change_event_t *ev = (void *)blk;
            int phys = (int8_t)ev->physical_wide;
            pidvd_spu_select_stream(e.spu, phys < 0 ? -1 : (phys & 0x1f));
            break;
        }
        case DVDNAV_HIGHLIGHT:
            update_highlight(&e, true);
            break;
        case DVDNAV_NAV_PACKET:
            update_highlight(&e, false);
            break;
        case DVDNAV_VTS_CHANGE: {
            uint32_t w = 0, h = 0;
            if (dvdnav_get_video_resolution(e.nav, &w, &h) == 0 && h) {
                pidvd_standard_t want = (h == 576 || h == 288)
                    ? PIDVD_STD_PAL : PIDVD_STD_NTSC;
                if (want != e.std) {
                    fprintf(stderr, "nav: VTS change -> %s\n",
                            pidvd_standard_name(want));
                    pidvd_video_set_standard(e.video, want);
                    e.std = want;
                }
            }
            pidvd_vdec_reset(e.vdec);
            pidvd_spu_clear(e.spu);
            break;
        }
        case DVDNAV_HOP_CHANNEL:
            pidvd_vdec_reset(e.vdec);
            pidvd_spu_clear(e.spu);
            break;
        case DVDNAV_CELL_CHANGE:
        case DVDNAV_AUDIO_STREAM_CHANGE:
            break;   /* audio: milestone 2b */
        case DVDNAV_STOP:
            running = false;
            break;
        default:
            break;
        }
        if (blk != mem)
            dvdnav_free_cache_block(e.nav, blk);
    }

    fprintf(stderr, "nav: stopped after %ld frames\n", e.frames);
    pidvd_input_close(e.input);
    long shown = pidvd_presenter_stop(e.pres);
    (void)shown;
    pidvd_vdec_free(e.vdec);
    pidvd_spu_free(e.spu);
    pidvd_video_close(e.video);
    free(e.fy);
    free(e.fu);
    free(e.fv);
    dvdnav_close(e.nav);
    return 0;
}
