#define _GNU_SOURCE   /* CPU_SET / pthread affinity */
#include "nav/engine.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>

#include <dvdnav/dvdnav.h>

#include "core/disc.h"
#include "decode/audio_a52.h"
#include "decode/spu.h"
#include "decode/video_mpeg2.h"
#include "demux/ps.h"
#include "platform/platform.h"
#include "present/presenter.h"
#include "sync/audio_playback.h"
#include "sync/av_sync.h"
#include "ui/draw.h"

#define FRAME_W 720
#define FRAME_H 576

#define ARING 128       /* four seconds: absorbs navigation/decode bursts */
#define AUDIO_STAGE 8   /* 256 ms maximum; normal prime consumes four chunks */

struct achunk {
    int16_t pcm[1536 * 2];
    int nframes;
    int rate;
    int64_t pts;
    int64_t video_start_pts;
    uint64_t epoch;
    uint64_t serial;
    bool gates_video;
};

struct engine {
    dvdnav_t *nav;
    pidvd_video_t *video;
    pidvd_presenter_t *pres;
    pidvd_vdec_t *vdec;
    pidvd_spu_t *spu;
    pidvd_input_t *input;
    pidvd_adec_t *adec;
    pidvd_ps_t ps;

    /* audio thread + ring */
    pthread_t audio_thread;
    pthread_mutex_t a_lock;
    pthread_cond_t a_not_empty, a_not_full;
    struct achunk aring[ARING];
    int a_head, a_tail, a_count;
    bool a_run;
    int cur_audio;          /* active AC-3 substream the demux decodes */
    int aud_user;           /* -1 follow VM, else user-chosen substream */
    int64_t vpts0;          /* first displayed video pts (start gate) */
    uint64_t epoch;
    uint64_t audio_serial;
    bool audio_gates_video;
    pidvd_av_sync_t *sync;

    pidvd_standard_t std;
    /* last decoded frame (planar), kept for re-pushing during stills */
    uint8_t *fy, *fu, *fv;
    int vw, vh;
    bool have_frame;
    pthread_mutex_t spu_lock;   /* spu state vs presenter blend thread */
    bool tff, rff;
    int64_t pts;            /* last decoded frame's pts, forwarded to presenter */
    char osd_text[16];      /* transient corner OSD text (guarded by spu_lock) */
    int osd_ticks;          /* frames the OSD stays up */
    pidvd_disc_t *disc;     /* IFO model for subtitle-track enumeration */
    uint32_t spu_seen;      /* physical SPU substreams seen in this title */
    int sub_user;           /* -2 follow VM, -1 off, else chosen substream */
    int64_t sub_clear_ns;   /* monotonic deadline to clear the subtitle, 0=none */
    bool paused;            /* nav-thread playback hold (PLAY/PAUSE) */
    long frames;
    /* producer timing */
    double t_prev, t_acc, t_max;
    long t_n;
    double nav_acc, dec_acc;   /* split: block fetch vs decode */
};

/* ---- compositing (presenter thread) ----------------------------------- */

/* Frames a volume change stays on screen (~1.5 s at the field-paced rate). */
#define OSD_FRAMES 48

/* Subtitle fallback: clear after this long if it carries no explicit stop. */
#define SUB_FALLBACK_NS (8LL * 1000000000LL)

static int64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* Transient corner chip, top-right. sy even keeps every edge interlace-safe
 * (draw.h interlace law); drawn straight into the XRGB frame the presenter is
 * about to scan out. */
static void draw_osd(uint8_t *rgb, int w, int h, const char *s)
{
    ui_canvas_t c = { rgb, w, h, w * 4 };
    int sx = 2, sy = 4, pad = 10, m = 40;   /* m: clear of CRT overscan */
    int tw = ui_text_w(s, sx), th = 8 * sy;
    int bx = w - tw - 2 * pad - m, by = m;
    if (bx < 0) bx = 0;
    ui_fill(&c, bx, by, tw + 2 * pad, th + 2 * pad, 0x101010);
    ui_hline2(&c, bx, by + th + 2 * pad - 2, tw + 2 * pad, 0xffa000);
    ui_text(&c, bx + pad, by + pad, sx, sy, 0xffa000, s);
    (void)h;
}

/* Post a corner OSD message (nav thread); read by the presenter under lock. */
static void osd_show(struct engine *e, const char *text)
{
    pthread_mutex_lock(&e->spu_lock);
    snprintf(e->osd_text, sizeof(e->osd_text), "%s", text);
    e->osd_ticks = OSD_FRAMES;
    pthread_mutex_unlock(&e->spu_lock);
}

static void blend_overlay(void *opaque, uint8_t *rgb, int w, int h)
{
    struct engine *e = opaque;
    pidvd_overlay_t ovl;
    pthread_mutex_lock(&e->spu_lock);
    if (e->sub_clear_ns && mono_ns() >= e->sub_clear_ns) {
        pidvd_spu_clear(e->spu);   /* duration elapsed with no successor */
        e->sub_clear_ns = 0;
    }
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
    int osd = 0;
    char osd_txt[16];
    if (e->osd_ticks > 0) {
        e->osd_ticks--;
        memcpy(osd_txt, e->osd_text, sizeof(osd_txt));
        osd = 1;
    }
    pthread_mutex_unlock(&e->spu_lock);

    if (osd)
        draw_osd(rgb, w, h, osd_txt);
}

static void prepare_epoch(void *ctx, uint64_t epoch)
{
    struct engine *e = ctx;
    /* Local DVD playback should reach this barrier quickly. A finite timeout
     * keeps silent, malformed, or unsupported streams from holding video. */
    pidvd_av_sync_wait_audio_ready(e->sync, epoch, 1500);
}

static void presented_frame(void *ctx, uint64_t epoch, int64_t pts,
                            const pidvd_video_stamp_t *stamp)
{
    struct engine *e = ctx;
    pidvd_av_sync_video_presented(e->sync, epoch, pts,
                                  stamp->monotonic_ns);
}

static uint64_t reset_audio_queue(struct engine *e, bool new_epoch,
                                  bool gate_video)
{
    pthread_mutex_lock(&e->a_lock);
    if (new_epoch)
        e->epoch++;
    e->audio_serial++;
    e->audio_gates_video = gate_video;
    uint64_t epoch = e->epoch;
    e->a_head = e->a_tail = e->a_count = 0;
    pthread_cond_broadcast(&e->a_not_empty);
    pthread_cond_broadcast(&e->a_not_full);
    pthread_mutex_unlock(&e->a_lock);
    return epoch;
}

static void reset_stream_epoch(struct engine *e)
{
    uint64_t epoch = reset_audio_queue(e, true, true);
    e->vpts0 = -1;
    e->have_frame = false;
    e->sub_user = -2;      /* new title: follow the VM until the user picks */
    e->aud_user = -1;      /* new title: follow the VM's default audio */
    e->spu_seen = 0;       /* re-discover this title's subtitle tracks */
    e->sub_clear_ns = 0;   /* drop any pending subtitle-clear deadline */
    pidvd_av_sync_reset(e->sync, epoch);
    pidvd_presenter_reset(e->pres);
}

static void reset_audio_stream(struct engine *e)
{
    bool video_started = pidvd_av_sync_video_is_started(e->sync, e->epoch);
    reset_audio_queue(e, false, !video_started);
}

static void push_composed(struct engine *e)
{
    /* Never feed the presenter while paused: it is intentionally not draining
     * its ring, so a push (e.g. a menu highlight repush) would fill the ring
     * and block the nav thread forever, deaf to the resume key. */
    if (!e->have_frame || e->paused)
        return;
    pidvd_presenter_push(e->pres, e->fy, e->fu, e->fv, e->vw, e->vh,
                         e->tff, e->rff, e->pts, e->epoch);
    e->frames++;
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static pidvd_standard_t initial_standard(const char *iso_path)
{
    pidvd_standard_t std = PIDVD_STD_NTSC;
    pidvd_disc_t *d = pidvd_disc_open(iso_path);
    if (!d) {
        fprintf(stderr, "nav: could not inspect disc standard; "
                        "starting in NTSC\n");
        return std;
    }

    bool mixed = false;
    std = pidvd_disc_standard(d, &mixed);
    fprintf(stderr, "nav: initial output -> %s%s\n",
            pidvd_standard_name(std), mixed ? " (mixed)" : "");
    pidvd_disc_close(d);
    return std;
}

static void on_frame(void *opaque, const uint8_t *y, const uint8_t *u,
                     const uint8_t *v, int w, int h, bool tff, bool rff,
                     int64_t pts)
{
    struct engine *e = opaque;
    if (e->vpts0 < 0 && pts >= 0)
        e->vpts0 = pts;
    double t = now_s();
    if (e->t_prev > 0) {
        double dt = t - e->t_prev;
        e->t_acc += dt;
        if (dt > e->t_max)
            e->t_max = dt;
        if (++e->t_n == 100) {
            fprintf(stderr, "decode: avg %.1fms max %.1fms per frame "
                    "(nav %.1fms dec %.1fms)\n",
                    e->t_acc / e->t_n * 1000.0, e->t_max * 1000.0,
                    e->nav_acc / e->t_n * 1000.0,
                    e->dec_acc / e->t_n * 1000.0);
            e->t_acc = e->t_max = 0;
            e->nav_acc = e->dec_acc = 0;
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
    e->pts = pts;
    e->have_frame = true;
    if (e->frames == 0) {
        FILE *up = fopen("/proc/uptime", "r");
        if (up) {
            double t = 0;
            if (fscanf(up, "%lf", &t) == 1)
                fprintf(stderr, "pidvd: first frame at %.1fs\n", t);
            fclose(up);
        }
    }
    push_composed(e);
}

static void on_video_es(void *opaque, const uint8_t *data, size_t len,
                        int64_t pts)
{
    struct engine *e = opaque;
    pidvd_vdec_push(e->vdec, data, len, pts);
}

static void on_spu_es(void *opaque, int substream, const uint8_t *data,
                      size_t len, int64_t pts)
{
    struct engine *e = opaque;
    (void)pts;   /* duration comes from the SPU's own DCSQ, not this PES pts */
    bool vts = dvdnav_is_domain_vts(e->nav);
    if (substream >= 0 && substream < 32 && vts)
        e->spu_seen |= 1u << substream;   /* a real subtitle track exists */
    pthread_mutex_lock(&e->spu_lock);
    pidvd_spu_packet(e->spu, substream, data, len);
    int64_t dur = 0;
    if (pidvd_spu_fresh(e->spu, &dur)) {
        /* A subtitle: clear it after its own duration (or the fallback). A menu
         * graphic (not a title): cancel any pending subtitle timeout so the
         * menu is never wiped. */
        e->sub_clear_ns = vts
            ? mono_ns() + (dur > 0 ? dur * 1000000000LL / 90000 : SUB_FALLBACK_NS)
            : 0;
    }
    pthread_mutex_unlock(&e->spu_lock);
}

static void on_audio_es(void *opaque, int substream, const uint8_t *data,
                        size_t len, int64_t pts)
{
    struct engine *e = opaque;
    if (substream != e->cur_audio)
        return;
    pidvd_adec_push(e->adec, data, len, pts);
}

/* Decoded AC-3 frames land in a lossless bounded queue. Four seconds of
 * capacity leaves ample burst tolerance; blocking is preferable to an audible
 * discontinuity and a permanently invalid output timeline. */
static void on_audio_pcm(void *opaque, const int16_t *frames, int nframes,
                         int rate, int64_t pts)
{
    struct engine *e = opaque;
    if (e->vpts0 < 0 || pts < 0)
        return;

    const int16_t *src = frames;
    int keep = nframes;
    int64_t kept_pts = pts;
    if (pts < e->vpts0) {
        int64_t early = e->vpts0 - pts;
        int trim = (int)((early * rate + 89999) / 90000);
        if (trim >= keep)
            return;
        src += (size_t)trim * 2;
        keep -= trim;
        kept_pts += (int64_t)trim * 90000 / rate;
    }

    pthread_mutex_lock(&e->a_lock);
    while (e->a_count == ARING && e->a_run)
        pthread_cond_wait(&e->a_not_full, &e->a_lock);
    if (!e->a_run) {
        pthread_mutex_unlock(&e->a_lock);
        return;
    }
    struct achunk *c = &e->aring[e->a_head];
    memcpy(c->pcm, src, (size_t)keep * 4);
    c->nframes = keep;
    c->rate = rate;
    c->pts = kept_pts;
    c->video_start_pts = e->audio_gates_video ? e->vpts0 : kept_pts;
    c->epoch = e->epoch;
    c->serial = e->audio_serial;
    c->gates_video = e->audio_gates_video;
    e->a_head = (e->a_head + 1) % ARING;
    e->a_count++;
    pthread_cond_signal(&e->a_not_empty);
    pthread_mutex_unlock(&e->a_lock);
}

static bool write_aligned_audio(pidvd_audio_playback_t *playback,
                                struct achunk *c, int64_t start_pts,
                                bool *first_chunk)
{
    if (*first_chunk) {
        int64_t end = c->pts + (int64_t)c->nframes * 90000 / c->rate;
        if (end <= start_pts)
            return true;
        if (c->pts < start_pts) {
            int trim = (int)(((start_pts - c->pts) * c->rate + 89999)
                             / 90000);
            if (trim >= c->nframes)
                return true;
            memmove(c->pcm, c->pcm + (size_t)trim * 2,
                    (size_t)(c->nframes - trim) * 4);
            c->nframes -= trim;
            c->pts += (int64_t)trim * 90000 / c->rate;
        }
        if (!pidvd_audio_playback_write_gap(playback, start_pts, c->pts))
            return false;
        *first_chunk = false;
    }
    return pidvd_audio_playback_write(playback, c->pcm,
                                      c->nframes, c->pts);
}

static void *audio_loop(void *opaque)
{
    struct engine *e = opaque;
    /* Keep the complete audio pipeline together and off the deadline-critical
     * presenter core. Nav/decode use CPUs 0-1; audio owns CPU 2, while video
     * presentation owns CPU 3 exclusively. */
    cpu_set_t cpus;
    CPU_ZERO(&cpus);
    CPU_SET(2, &cpus);
    pthread_setaffinity_np(pthread_self(), sizeof(cpus), &cpus);
    struct sched_param sp = { .sched_priority = 30 };
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);

    pidvd_audio_playback_t *playback = NULL;
    uint64_t local_epoch = 0;
    uint64_t local_serial = 0;
    bool disabled = false;
    bool first_chunk = true;
    int64_t alignment_pts = -1;
    bool alignment_follows_display = false;
    int reprimes = 0;
    struct achunk staged[AUDIO_STAGE];
    int staged_count = 0, staged_frames = 0, staged_rate = 0;

    for (;;) {
        pthread_mutex_lock(&e->a_lock);
        while (e->a_count == 0 && e->a_run
               && local_serial == e->audio_serial)
            pthread_cond_wait(&e->a_not_empty, &e->a_lock);
        if (!e->a_run) {
            pthread_mutex_unlock(&e->a_lock);
            break;
        }
        if (local_serial != e->audio_serial) {
            local_serial = e->audio_serial;
            local_epoch = e->epoch;
            pthread_mutex_unlock(&e->a_lock);
            pidvd_audio_playback_free(playback, true);
            playback = NULL;
            disabled = false;
            first_chunk = true;
            alignment_pts = -1;
            alignment_follows_display = false;
            reprimes = 0;
            staged_count = staged_frames = staged_rate = 0;
            continue;
        }
        struct achunk c = e->aring[e->a_tail];
        e->a_tail = (e->a_tail + 1) % ARING;
        e->a_count--;
        pthread_cond_signal(&e->a_not_full);
        pthread_mutex_unlock(&e->a_lock);

        if (c.epoch != local_epoch || c.serial != local_serial)
            continue;
        if (disabled)
            continue;

        /* Coarse startup catch-up. When video is already running while we are
         * still priming — the startup gate timed out, or this is a re-prime —
         * the queue holds stale audio from the epoch's start that is now well
         * behind the picture. Writing it would force the slew-limited servo to
         * fast-forward for a minute, draining the buffer to an underrun. Drop
         * whole chunks that trail the display clock so audio primes at the live
         * point and starts already in sync; the servo only trims the rest. */
        if (!playback
            && pidvd_av_sync_video_is_started(e->sync, local_epoch)) {
            int64_t disp;
            int64_t chunk_end = c.pts + (int64_t)c.nframes * 90000 / c.rate;
            if (pidvd_av_sync_display_now(e->sync, local_epoch, &disp)
                && chunk_end < disp - 9000 /* >100 ms behind the picture */) {
                staged_count = staged_frames = staged_rate = 0;
                continue;
            }
        }

        if (playback && pidvd_audio_playback_rate(playback) != c.rate) {
            pidvd_audio_playback_free(playback, true);
            playback = NULL;
            first_chunk = true;
            alignment_pts = -1;
            alignment_follows_display = false;
            staged_count = staged_frames = staged_rate = 0;
            c.gates_video = false; /* video is already running at a rate change */
        }

        if (!playback) {
            if (staged_count && staged_rate != c.rate) {
                staged_count = staged_frames = 0;
                c.gates_video = false;
            }
            if (staged_count == AUDIO_STAGE) {
                /* The normal four-chunk prime cannot reach this. If malformed
                 * timing does, retain the newest bounded window. */
                int dropped = staged[0].nframes;
                memmove(staged, staged + 1,
                        (AUDIO_STAGE - 1) * sizeof(staged[0]));
                staged_count--;
                staged_frames -= dropped;
            }
            staged[staged_count++] = c;
            staged_frames += c.nframes;
            staged_rate = c.rate;
            if (staged_frames
                < pidvd_audio_playback_prime_frames(staged_rate))
                continue;

            /* If video is already running — either we never gated it, or the
             * startup gate timed out before audio primed on a slow disc —
             * start audio at the live display position instead of the epoch's
             * first PTS. Otherwise audio begins however many hundred ms behind
             * and the slew-limited servo rails for a minute clawing it back,
             * starving the buffer (audible dropouts). Skipping the stale audio
             * to the display clock starts it already in sync. */
            bool video_running =
                pidvd_av_sync_video_is_started(e->sync, local_epoch);
            alignment_pts = staged[0].video_start_pts;
            alignment_follows_display =
                !staged[0].gates_video || video_running;
            if (alignment_follows_display) {
                int64_t display_pts;
                if (pidvd_av_sync_display_now(e->sync, local_epoch,
                                              &display_pts))
                    alignment_pts = display_pts;
            }
            playback = pidvd_audio_playback_new(e->sync,
                                                local_epoch, staged_rate);
            if (!playback) {
                fprintf(stderr, "audio: output unavailable, muting\n");
                pidvd_av_sync_audio_ready(e->sync, local_epoch, false);
                disabled = true;
                staged_count = staged_frames = staged_rate = 0;
                continue;
            }
            bool ok = true;
            for (int i = 0; i < staged_count && ok; i++)
                ok = write_aligned_audio(playback, &staged[i],
                                         alignment_pts, &first_chunk);
            staged_count = staged_frames = staged_rate = 0;
            if (ok) {
                reprimes = 0;   /* a clean (re)prime clears the failure count */
                continue;
            }
        } else {
            if (first_chunk && alignment_follows_display) {
                int64_t display_pts;
                if (pidvd_av_sync_display_now(e->sync, local_epoch,
                                              &display_pts))
                    alignment_pts = display_pts;
            }
            bool ok = write_aligned_audio(playback, &c, alignment_pts,
                                          &first_chunk);
            if (ok)
                continue;
        }

        /* A path reaching here failed the output timeline (e.g. an ALSA
         * underrun recovery could not paper over). Flush and re-prime rather
         * than muting the rest of the title; the re-prime aligns to the live
         * display clock, so audio simply resumes in sync. Give up only if it
         * keeps failing, which indicates the device itself is gone. */
        pidvd_audio_playback_free(playback, true);
        playback = NULL;
        first_chunk = true;
        alignment_pts = -1;
        alignment_follows_display = false;
        staged_count = staged_frames = staged_rate = 0;
        if (++reprimes > 8) {
            fprintf(stderr, "audio: output keeps failing, muting\n");
            pidvd_av_sync_audio_ready(e->sync, local_epoch, false);
            disabled = true;
        }
    }
    pidvd_audio_playback_free(playback, false);
    return NULL;
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

/* The IFO title record dvdnav is currently playing, or NULL. */
static const pidvd_title_t *cur_title(struct engine *e)
{
    if (!e->disc)
        return NULL;
    int32_t title = 0, part = 0;
    if (dvdnav_current_title_info(e->nav, &title, &part) != DVDNAV_STATUS_OK
        || title < 1)
        return NULL;
    for (int i = 0; i < pidvd_disc_title_count(e->disc); i++) {
        const pidvd_title_t *t = pidvd_disc_title(e->disc, i);
        if (t->title_nr == title)
            return t;
    }
    return NULL;
}

/* Cycle the subtitle over this title's tracks: off -> 1 -> 2 -> ... -> off.
 * The track list comes from the disc IFO, so any track is selectable
 * immediately — before its first subtitle has been demuxed — and is labelled
 * by language. Falls back to the substreams actually seen if the IFO model is
 * unavailable. Title domain only (menus own the SPU); the user's choice
 * overrides the VM until a menu resets it (see DVDNAV_SPU_STREAM_CHANGE). */
static void cycle_subtitle(struct engine *e)
{
    if (!dvdnav_is_domain_vts(e->nav))
        return;

    int phys[PIDVD_MAX_SUBP], n = 0;
    const char *lang[PIDVD_MAX_SUBP];
    const pidvd_title_t *t = cur_title(e);
    if (t && t->n_subp > 0) {
        for (int i = 0; i < t->n_subp; i++) {
            phys[n] = t->subp[i].phys;
            lang[n] = t->subp[i].lang;
            n++;
        }
    } else {
        for (int i = 0; i < 32; i++)
            if (e->spu_seen & (1u << i)) {
                phys[n] = i;
                lang[n] = "--";
                n++;
            }
    }
    if (n == 0) {
        osd_show(e, "NO SUBS");
        return;
    }

    int next, ni = -1;                   /* ni: index of next in phys[] */
    if (e->sub_user < 0) {               /* off / follow -> first track */
        next = phys[0];
        ni = 0;
    } else {
        int idx = -1;
        for (int i = 0; i < n; i++)
            if (phys[i] == e->sub_user) { idx = i; break; }
        if (idx >= 0 && idx + 1 < n) { next = phys[idx + 1]; ni = idx + 1; }
        else next = -1;                  /* past last -> off */
    }
    e->sub_user = next;
    pthread_mutex_lock(&e->spu_lock);
    pidvd_spu_select_stream(e->spu, next);
    pthread_mutex_unlock(&e->spu_lock);

    if (next < 0) {
        osd_show(e, "SUB OFF");
        return;
    }
    char b[16];
    const char *L = lang[ni];
    if (L && L[0] && L[0] != '-')
        snprintf(b, sizeof(b), "SUB %c%c", toupper((unsigned char)L[0]),
                 toupper((unsigned char)L[1]));
    else
        snprintf(b, sizeof(b), "SUB %d", ni + 1);
    osd_show(e, b);
}

/* OSD label for an audio track: "AUD <LANG> <ch>CH", or "AUD <n> <ch>CH" when
 * the IFO gave no language (so same-language tracks stay distinguishable by
 * their channel count). buf must hold >= 16 bytes. */
static void aud_label(char *buf, const pidvd_audio_stream_t *as, int pos1)
{
    if (as->lang[0] && as->lang[0] != '-')
        snprintf(buf, 16, "AUD %c%c %dCH", toupper((unsigned char)as->lang[0]),
                 toupper((unsigned char)as->lang[1]), as->channels);
    else
        snprintf(buf, 16, "AUD %d %dCH", pos1, as->channels);
}

/* Cycle the audio over this title's tracks: 1 -> 2 -> ... -> 1 (no off). The
 * track list + languages come from the IFO; title domain only. The user's
 * choice overrides the VM until a menu resets it (DVDNAV_AUDIO_STREAM_CHANGE). */
static void cycle_audio(struct engine *e)
{
    if (!dvdnav_is_domain_vts(e->nav))
        return;
    const pidvd_title_t *t = cur_title(e);
    if (!t || t->n_audio < 1)
        return;
    char b[16];
    if (t->n_audio == 1) {                 /* nothing to switch to */
        aud_label(b, &t->audio[0], 1);
        osd_show(e, b);
        return;
    }
    int idx = -1;
    for (int i = 0; i < t->n_audio; i++)
        if (t->audio[i].phys == e->cur_audio) { idx = i; break; }
    int ni = (idx >= 0) ? (idx + 1) % t->n_audio : 0;
    e->aud_user = t->audio[ni].phys;
    e->cur_audio = t->audio[ni].phys;
    pidvd_adec_reset(e->adec);
    reset_audio_stream(e);
    aud_label(b, &t->audio[ni], ni + 1);
    osd_show(e, b);
}

/* Freeze or resume playback. Pause holds the displayed frame in the presenter
 * (the KMS scanout keeps it on screen), which stops the av-sync clock, and
 * drops audio output so sound stops promptly; the nav loop then parks polling
 * input. Resume re-anchors the clock past the elapsed pause and lets the audio
 * thread re-prime to the live display position. */
static void set_paused(struct engine *e, bool paused)
{
    e->paused = paused;
    if (paused) {
        pidvd_presenter_set_paused(e->pres, true);
        reset_audio_stream(e);
    } else {
        pidvd_av_sync_resume(e->sync, e->epoch);
        pidvd_presenter_set_paused(e->pres, false);
    }
}

/* Brief "T1 CH 3/26" indicator after a chapter/title jump. Title domain only. */
static void show_chapter_osd(struct engine *e)
{
    if (!dvdnav_is_domain_vts(e->nav))
        return;
    int32_t title = 0, part = 0, parts = 0;
    if (dvdnav_current_title_info(e->nav, &title, &part) != DVDNAV_STATUS_OK
        || title < 1)
        return;
    char b[16];
    if (dvdnav_get_number_of_parts(e->nav, title, &parts) == DVDNAV_STATUS_OK
        && parts > 0)
        snprintf(b, sizeof(b), "T%d CH %d/%d", title, part, parts);
    else
        snprintf(b, sizeof(b), "T%d CH %d", title, part);
    osd_show(e, b);
}

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
    case PIDVD_KEY_NEXT_CHAPTER: dvdnav_next_pg_search(e->nav);
                                 show_chapter_osd(e); break;
    case PIDVD_KEY_PREV_CHAPTER: dvdnav_prev_pg_search(e->nav);
                                 show_chapter_osd(e); break;
    case PIDVD_KEY_STOP:   return false;
    case PIDVD_KEY_FIELD:  pidvd_video_toggle_field_parity(e->video); break;
    case PIDVD_KEY_VOL_UP:
    case PIDVD_KEY_VOL_DOWN: {
        int v = pidvd_audio_adjust_volume(k == PIDVD_KEY_VOL_UP ? +5 : -5);
        char b[16];
        snprintf(b, sizeof(b), "VOL %d%%", v);
        osd_show(e, b);
        break;
    }
    case PIDVD_KEY_SUBTITLE: cycle_subtitle(e); break;
    case PIDVD_KEY_AUDIO:    cycle_audio(e); break;
    case PIDVD_KEY_PLAY_PAUSE: set_paused(e, !e->paused); break;
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
    /* IFO model alongside dvdnav, for up-front subtitle-track enumeration
     * (non-fatal: cycle_subtitle falls back to seen substreams without it). */
    e.disc = pidvd_disc_open(iso_path);

    e.std = initial_standard(iso_path);
    e.video = pidvd_video_open(e.std);
    if (!e.video) {
        if (e.disc)
            pidvd_disc_close(e.disc);
        dvdnav_close(e.nav);
        return 1;
    }
    /* nav+decode use cores 0-1; CPU 2 owns resampling/ALSA and CPU 3 owns
     * presentation. Neither support pipeline can preempt the video core. */
    cpu_set_t cpus;
    CPU_ZERO(&cpus);
    CPU_SET(0, &cpus); CPU_SET(1, &cpus);
    pthread_setaffinity_np(pthread_self(), sizeof(cpus), &cpus);

    pthread_mutex_init(&e.spu_lock, NULL);
    pthread_mutex_init(&e.a_lock, NULL);
    pthread_cond_init(&e.a_not_empty, NULL);
    pthread_cond_init(&e.a_not_full, NULL);
    e.sync = pidvd_av_sync_new();
    e.epoch = 1;
    e.audio_serial = 1;
    e.audio_gates_video = true;
    pidvd_av_sync_reset(e.sync, e.epoch);
    pidvd_frame_t *dims = pidvd_video_begin_frame(e.video);
    e.pres = pidvd_presenter_start(e.video, dims->width, dims->height,
                                   blend_overlay, &e, prepare_epoch,
                                   presented_frame, &e);
    e.vdec = pidvd_vdec_new(on_frame, &e);
    e.spu = pidvd_spu_new();
    e.adec = pidvd_adec_new(on_audio_pcm, &e);
    e.input = pidvd_input_open();
    e.vpts0 = -1;
    e.cur_audio = 0;
    e.aud_user = -1;   /* audio follows the disc until the user cycles AUDIO */
    e.sub_user = -2;   /* subtitles follow the disc until the user cycles SUB */
    e.a_run = true;
    pthread_create(&e.audio_thread, NULL, audio_loop, &e);
    e.fy = calloc(1, FRAME_W * FRAME_H);
    e.fu = calloc(1, FRAME_W * FRAME_H / 4);
    e.fv = calloc(1, FRAME_W * FRAME_H / 4);
    pidvd_ps_init(&e.ps, on_video_es, &e);
    e.ps.spu_cb = on_spu_es;
    e.ps.audio_cb = on_audio_es;

    bool running = true;
    unsigned tick = 0;
    while (running) {
        if ((tick++ & 63) == 0) {
            pidvd_key_t k = pidvd_input_poll(e.input);
            if (k != PIDVD_KEY_NONE && !handle_key(&e, k))
                break;
        }

        /* Paused: hold here polling input (the presenter holds the frame and
         * audio is stopped) until PLAY/PAUSE resumes or STOP ends playback.
         * Only transport keys act while paused — other keys (nav/menu/AUDIO/
         * SUB/VOL) are ignored, since the frozen presenter cannot render their
         * feedback and a menu repush would wedge the nav thread. */
        while (e.paused && running) {
            pidvd_key_t k = pidvd_input_poll(e.input);
            if ((k == PIDVD_KEY_PLAY_PAUSE || k == PIDVD_KEY_STOP)
                && !handle_key(&e, k)) {
                running = false;
                break;
            }
            if (e.paused)
                usleep(20000);
        }
        if (!running)
            break;

        /* cache blocks avoid a per-sector memcpy out of dvdnav */
        uint8_t *blk = mem;
        int32_t event = 0, len = 0;
        double t0 = now_s();
        if (dvdnav_get_next_cache_block(e.nav, &blk, &event, &len)
            != DVDNAV_STATUS_OK) {
            fprintf(stderr, "nav: %s\n", dvdnav_err_to_string(e.nav));
            break;
        }

        double t1 = now_s();
        e.nav_acc += t1 - t0;

        switch (event) {
        case DVDNAV_BLOCK_OK:
            pidvd_ps_push_sector(&e.ps, blk);
            e.dec_acc += now_s() - t1;
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
            int vm = phys < 0 ? -1 : (phys & 0x1f);
            if (!dvdnav_is_domain_vts(e.nav))
                e.sub_user = -2;   /* menus: the VM owns the SPU stream */
            if (e.sub_user == -2) {   /* otherwise the user's choice stands */
                pthread_mutex_lock(&e.spu_lock);
                pidvd_spu_select_stream(e.spu, vm);
                pthread_mutex_unlock(&e.spu_lock);
            }
            break;
        }
        case DVDNAV_HIGHLIGHT:
            update_highlight(&e, true);
            break;
        case DVDNAV_NAV_PACKET:
            update_highlight(&e, false);
            break;
        case DVDNAV_VTS_CHANGE: {
            pidvd_vdec_reset(e.vdec);
            pidvd_adec_reset(e.adec);
            pidvd_spu_clear(e.spu);
            /* Quiesce presentation before touching KMS mode state. */
            reset_stream_epoch(&e);
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
            break;
        }
        case DVDNAV_HOP_CHANNEL:
            pidvd_vdec_reset(e.vdec);
            pidvd_adec_reset(e.adec);
            pidvd_spu_clear(e.spu);
            reset_stream_epoch(&e);
            break;
        case DVDNAV_AUDIO_STREAM_CHANGE: {
            dvdnav_audio_stream_change_event_t *ev = (void *)blk;
            int phys = (int8_t)ev->physical;
            if (!dvdnav_is_domain_vts(e.nav))
                e.aud_user = -1;   /* menus: follow the VM */
            if (e.aud_user < 0 && phys >= 0 && (phys & 7) != e.cur_audio) {
                e.cur_audio = phys & 7;
                pidvd_adec_reset(e.adec);
                reset_audio_stream(&e);
            }
            break;
        }
        case DVDNAV_CELL_CHANGE:
            break;
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
    pthread_mutex_lock(&e.a_lock);
    e.a_run = false;
    pthread_cond_broadcast(&e.a_not_empty);
    pthread_cond_broadcast(&e.a_not_full);
    pthread_mutex_unlock(&e.a_lock);
    pidvd_av_sync_audio_ready(e.sync, e.epoch, false);
    pthread_join(e.audio_thread, NULL);
    pidvd_adec_free(e.adec);
    pidvd_input_close(e.input);
    long shown = pidvd_presenter_stop(e.pres);
    (void)shown;
    pidvd_vdec_free(e.vdec);
    pidvd_spu_free(e.spu);
    pidvd_av_sync_free(e.sync);
    pidvd_video_close(e.video);
    free(e.fy);
    free(e.fu);
    free(e.fv);
    if (e.disc)
        pidvd_disc_close(e.disc);
    dvdnav_close(e.nav);
    return 0;
}
