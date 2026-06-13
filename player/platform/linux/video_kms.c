/* KMS/DRM video output: native interlaced scanout on the VEC
 * (Composite-1). Double-buffered dumb buffers, one page flip per frame
 * (two fields); flip completion paces the caller at frame rate. */
#include "platform/platform.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

struct kms_buf {
    uint32_t handle;
    uint32_t fb_id;
    uint32_t pitch;
    size_t   size;
    uint8_t *map;
};

struct pidvd_video {
    int fd;
    uint32_t conn_id;
    uint32_t crtc_id;
    int crtc_index;
    unsigned field_parity;   /* vblank-sequence parity flips latch on */
    bool progressive;        /* 240p/288p menu mode (decimated scanout) */
    drmModeModeInfo mode;
    struct kms_buf buf[2];
    uint8_t *scratch;        /* full-height render target (progressive) */
    int scratch_h;           /* 2 * vdisplay when progressive, else 0 */
    int back;            /* index handed out by begin_frame */
    bool flip_pending;
    pidvd_frame_t frame;
    /* flip diagnostics */
    unsigned last_flip_seq;
    long flips;
    long delta_hist[6];   /* fields between flips: [0]=other, 1..5 */
    double t_last;
};

static int create_buf(int fd, int w, int h, struct kms_buf *b)
{
    struct drm_mode_create_dumb creq = { .width = w, .height = h, .bpp = 32 };
    if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0)
        return -1;
    b->handle = creq.handle;
    b->pitch = creq.pitch;
    b->size = creq.size;
    if (drmModeAddFB(fd, w, h, 24, 32, creq.pitch, creq.handle, &b->fb_id) < 0)
        return -1;
    struct drm_mode_map_dumb mreq = { .handle = creq.handle };
    if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0)
        return -1;
    b->map = mmap(NULL, creq.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                  fd, mreq.offset);
    if (b->map == MAP_FAILED)
        return -1;
    memset(b->map, 0, b->size);
    return 0;
}

/* Find a Composite mode for the requested standard and scan. Interlaced:
 * 480i/576i. Progressive: 240p/288p (half the active lines, no interlace
 * flag). Sets v->mode/conn/crtc on success. */
static bool pick_mode(pidvd_video_t *v, pidvd_standard_t std, bool progressive)
{
    int want_v = progressive ? ((std == PIDVD_STD_PAL) ? 288 : 240)
                             : ((std == PIDVD_STD_PAL) ? 576 : 480);
    drmModeRes *res = drmModeGetResources(v->fd);
    if (!res)
        return false;
    bool found = false;
    for (int i = 0; i < res->count_connectors && !found; i++) {
        drmModeConnector *c = drmModeGetConnector(v->fd, res->connectors[i]);
        if (!c)
            continue;
        if (c->connector_type == DRM_MODE_CONNECTOR_Composite
            && c->connection == DRM_MODE_CONNECTED) {
            for (int m = 0; m < c->count_modes; m++) {
                drmModeModeInfo *mi = &c->modes[m];
                bool mi_int = (mi->flags & DRM_MODE_FLAG_INTERLACE) != 0;
                if (mi_int == !progressive && mi->vdisplay == want_v) {
                    v->conn_id = c->connector_id;
                    v->mode = *mi;
                    /* take the encoder's CRTC, or the first one */
                    drmModeEncoder *e = c->encoder_id
                        ? drmModeGetEncoder(v->fd, c->encoder_id) : NULL;
                    v->crtc_id = (e && e->crtc_id) ? e->crtc_id
                                                   : res->crtcs[0];
                    if (e)
                        drmModeFreeEncoder(e);
                    for (int k = 0; k < res->count_crtcs; k++)
                        if (res->crtcs[k] == v->crtc_id)
                            v->crtc_index = k;
                    found = true;
                    break;
                }
            }
        }
        drmModeFreeConnector(c);
    }
    drmModeFreeResources(res);
    return found;
}

static void free_buffers(pidvd_video_t *v)
{
    for (int i = 0; i < 2; i++) {
        struct kms_buf *b = &v->buf[i];
        if (b->map && b->map != MAP_FAILED)
            munmap(b->map, b->size);
        if (b->fb_id)
            drmModeRmFB(v->fd, b->fb_id);
        if (b->handle) {
            struct drm_mode_destroy_dumb dreq = { .handle = b->handle };
            ioctl(v->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
        }
        memset(b, 0, sizeof(*b));
    }
    free(v->scratch);
    v->scratch = NULL;
    v->scratch_h = 0;
}

/* Allocate scanout buffers for the current mode (plus a full-height
 * scratch when progressive) and program the CRTC. */
static bool setup_mode(pidvd_video_t *v)
{
    for (int i = 0; i < 2; i++)
        if (create_buf(v->fd, v->mode.hdisplay, v->mode.vdisplay,
                       &v->buf[i]) < 0) {
            fprintf(stderr, "video: dumb buffer: %s\n", strerror(errno));
            return false;
        }
    if (v->progressive) {
        v->scratch_h = v->mode.vdisplay * 2;
        v->scratch = calloc((size_t)v->mode.hdisplay * v->scratch_h, 4);
        if (!v->scratch)
            return false;
    }
    if (drmModeSetCrtc(v->fd, v->crtc_id, v->buf[0].fb_id, 0, 0,
                       &v->conn_id, 1, &v->mode) < 0) {
        fprintf(stderr, "video: modeset failed: %s\n", strerror(errno));
        return false;
    }
    v->back = 1;
    return true;
}

/* Pick std/scan, with automatic fallback from progressive to interlaced
 * when the VEC doesn't expose a 240p/288p mode. Updates v->progressive. */
static bool select_mode(pidvd_video_t *v, pidvd_standard_t std,
                        pidvd_scan_t scan)
{
    bool want_prog = (scan == PIDVD_SCAN_PROGRESSIVE);
    if (want_prog && pick_mode(v, std, true)) {
        v->progressive = true;
        return true;
    }
    if (want_prog)
        fprintf(stderr, "video: no progressive %s mode on Composite-1, "
                        "falling back to interlaced\n",
                pidvd_standard_name(std));
    if (pick_mode(v, std, false)) {
        v->progressive = false;
        return true;
    }
    return false;
}

pidvd_video_t *pidvd_video_open_mode(pidvd_standard_t std, pidvd_scan_t scan)
{
    pidvd_video_t *v = calloc(1, sizeof(*v));
    v->fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (v->fd < 0) {
        fprintf(stderr, "video: cannot open /dev/dri/card0: %s\n",
                strerror(errno));
        goto fail;
    }
    if (!select_mode(v, std, scan)) {
        fprintf(stderr, "video: no connected %s mode on Composite-1\n",
                pidvd_standard_name(std));
        goto fail;
    }
    if (!setup_mode(v))
        goto fail;
    const char *par = getenv("PIDVD_FIELD_PARITY");
    v->field_parity = (par && par[0] == '1') ? 1 : 0;
    fprintf(stderr, "video: %s %ux%u%s on Composite-1, field parity %u\n",
            v->mode.name, v->mode.hdisplay, v->mode.vdisplay,
            v->progressive ? "p" : "i", v->field_parity);
    return v;
fail:
    pidvd_video_close(v);
    return NULL;
}

pidvd_video_t *pidvd_video_open(pidvd_standard_t std)
{
    return pidvd_video_open_mode(std, PIDVD_SCAN_INTERLACED);
}

bool pidvd_video_set_mode(pidvd_video_t *v, pidvd_standard_t std,
                          pidvd_scan_t scan)
{
    free_buffers(v);
    if (!select_mode(v, std, scan) || !setup_mode(v))
        return false;
    fprintf(stderr, "video: switched to %s %ux%u%s\n", v->mode.name,
            v->mode.hdisplay, v->mode.vdisplay, v->progressive ? "p" : "i");
    return true;
}

bool pidvd_video_set_standard(pidvd_video_t *v, pidvd_standard_t std)
{
    return pidvd_video_set_mode(v, std, PIDVD_SCAN_INTERLACED);
}

pidvd_frame_t *pidvd_video_begin_frame(pidvd_video_t *v)
{
    if (v->progressive) {
        /* Caller renders full height; present() decimates 2:1 into the
         * half-height scanout buffer. Render stays scan-agnostic. */
        v->frame.pixels = v->scratch;
        v->frame.width = v->mode.hdisplay;
        v->frame.height = v->scratch_h;
        v->frame.stride = v->mode.hdisplay * 4;
        return &v->frame;
    }
    struct kms_buf *b = &v->buf[v->back];
    v->frame.pixels = b->map;
    v->frame.width = v->mode.hdisplay;
    v->frame.height = v->mode.vdisplay;
    v->frame.stride = (int)b->pitch;
    return &v->frame;
}

struct flip_evt { bool pending; unsigned seq; };

static void flip_done(int fd, unsigned frame, unsigned sec, unsigned usec,
                      void *data)
{
    struct flip_evt *e = data;
    (void)fd; (void)sec; (void)usec;
    e->seq = frame;
    e->pending = false;
}

bool pidvd_video_present(pidvd_video_t *v, pidvd_frame_t *f,
                         bool tff, bool rff)
{
    (void)f;
    /* Field-parity-locked flips. With the kernel patches (0001/0002)
     * the vblank sequence ticks once per field and its parity IS the
     * hardware field identity. Latch every flip on the configured
     * parity: each weaved frame then starts on the same (top) field —
     * 2 fields per frame, 25fps, field-exact for 2:2 content.
     * TODO(field-sched): rff (NTSC 3:2) = hold one extra field. */
    (void)tff; (void)rff;

    if (v->progressive) {
        /* Decimate the full-height render into the half-height scanout
         * buffer: every other line. The menu is interlace-safe (no
         * sub-2-line feature), so this is lossless for its content and
         * yields a crisp native 240p/288p image. No field parity in a
         * progressive mode — one flip per vblank. */
        struct kms_buf *b = &v->buf[v->back];
        int bpl = v->mode.hdisplay * 4;
        for (int y = 0; y < v->mode.vdisplay; y++)
            memcpy(b->map + (size_t)y * b->pitch,
                   v->scratch + (size_t)(2 * y) * bpl, (size_t)bpl);
    } else {
        uint32_t crtcbits =
            (uint32_t)v->crtc_index << DRM_VBLANK_HIGH_CRTC_SHIFT;
        drmVBlank w = {
            .request = { .type = DRM_VBLANK_RELATIVE | crtcbits,
                         .sequence = 1 },
        };
        if (drmWaitVBlank(v->fd, &w) == 0
            && ((w.reply.sequence + 1) & 1) != v->field_parity) {
            drmVBlank w2 = {
                .request = { .type = DRM_VBLANK_RELATIVE | crtcbits,
                             .sequence = 1 },
            };
            drmWaitVBlank(v->fd, &w2);
        }
    }

    struct flip_evt evt = { .pending = true };
    if (drmModePageFlip(v->fd, v->crtc_id, v->buf[v->back].fb_id,
                        DRM_MODE_PAGE_FLIP_EVENT, &evt) < 0)
        return false;

    drmEventContext ev = {
        .version = 2,
        .page_flip_handler = flip_done,
    };
    while (evt.pending) {
        struct pollfd p = { .fd = v->fd, .events = POLLIN };
        if (poll(&p, 1, 1000) <= 0)
            return false;
        drmHandleEvent(v->fd, &ev);
    }
    v->back ^= 1;

    unsigned delta = evt.seq - v->last_flip_seq;
    v->last_flip_seq = evt.seq;
    v->delta_hist[delta <= 5 ? delta : 0]++;
    if (++v->flips % 50 == 0) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double t = now.tv_sec + now.tv_nsec / 1e9;
        double fps = v->t_last > 0 ? 50.0 / (t - v->t_last) : 0;
        v->t_last = t;
        fprintf(stderr, "video: flips=%ld fps=%.2f seq-deltas 1:%ld 2:%ld "
                "3:%ld 4:%ld 5:%ld other:%ld (seq %u)\n",
                v->flips, fps, v->delta_hist[1], v->delta_hist[2],
                v->delta_hist[3], v->delta_hist[4], v->delta_hist[5],
                v->delta_hist[0], evt.seq);
    }
    return true;
}

void pidvd_video_vblprobe(pidvd_video_t *v, int n)
{
    uint32_t crtcbits =
        (uint32_t)v->crtc_index << DRM_VBLANK_HIGH_CRTC_SHIFT;
    unsigned prev = 0;
    long prev_us = 0;
    for (int i = 0; i < n; i++) {
        drmVBlank w = {
            .request = { .type = DRM_VBLANK_RELATIVE | crtcbits,
                         .sequence = 1 },
        };
        if (drmWaitVBlank(v->fd, &w) != 0) {
            fprintf(stderr, "vblprobe: wait failed\n");
            return;
        }
        long us = (long)w.reply.tval_sec * 1000000 + w.reply.tval_usec;
        fprintf(stderr, "vblprobe: seq=%u (+%u) dt=%ldus\n",
                w.reply.sequence, w.reply.sequence - prev,
                i ? (us - prev_us) : 0);
        prev = w.reply.sequence;
        prev_us = us;
    }
}

void pidvd_video_close(pidvd_video_t *v)
{
    if (!v)
        return;
    if (v->fd >= 0)
        free_buffers(v);
    if (v->fd >= 0)
        close(v->fd);
    free(v);
}
