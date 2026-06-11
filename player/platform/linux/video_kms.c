/* KMS/DRM video output: native interlaced scanout on the VEC
 * (Composite-1). Double-buffered dumb buffers, one page flip per frame
 * (two fields); flip completion paces the caller at frame rate. */
#include "platform/platform.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
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
    drmModeModeInfo mode;
    struct kms_buf buf[2];
    int back;            /* index handed out by begin_frame */
    bool flip_pending;
    pidvd_frame_t frame;
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

static bool pick_mode(pidvd_video_t *v, pidvd_standard_t std)
{
    int want_v = (std == PIDVD_STD_PAL) ? 576 : 480;
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
                if ((mi->flags & DRM_MODE_FLAG_INTERLACE)
                    && mi->vdisplay == want_v) {
                    v->conn_id = c->connector_id;
                    v->mode = *mi;
                    /* take the encoder's CRTC, or the first one */
                    drmModeEncoder *e = c->encoder_id
                        ? drmModeGetEncoder(v->fd, c->encoder_id) : NULL;
                    v->crtc_id = (e && e->crtc_id) ? e->crtc_id
                                                   : res->crtcs[0];
                    if (e)
                        drmModeFreeEncoder(e);
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

pidvd_video_t *pidvd_video_open(pidvd_standard_t std)
{
    pidvd_video_t *v = calloc(1, sizeof(*v));
    v->fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (v->fd < 0) {
        fprintf(stderr, "video: cannot open /dev/dri/card0: %s\n",
                strerror(errno));
        goto fail;
    }
    if (!pick_mode(v, std)) {
        fprintf(stderr, "video: no connected interlaced %s mode on "
                        "Composite-1\n", pidvd_standard_name(std));
        goto fail;
    }
    for (int i = 0; i < 2; i++)
        if (create_buf(v->fd, v->mode.hdisplay, v->mode.vdisplay,
                       &v->buf[i]) < 0) {
            fprintf(stderr, "video: dumb buffer: %s\n", strerror(errno));
            goto fail;
        }
    if (drmModeSetCrtc(v->fd, v->crtc_id, v->buf[0].fb_id, 0, 0,
                       &v->conn_id, 1, &v->mode) < 0) {
        fprintf(stderr, "video: modeset failed: %s\n", strerror(errno));
        goto fail;
    }
    fprintf(stderr, "video: %s %ux%u%s on Composite-1\n",
            v->mode.name, v->mode.hdisplay, v->mode.vdisplay,
            (v->mode.flags & DRM_MODE_FLAG_INTERLACE) ? "i" : "p");
    v->back = 1;
    return v;
fail:
    pidvd_video_close(v);
    return NULL;
}

bool pidvd_video_set_standard(pidvd_video_t *v, pidvd_standard_t std)
{
    if (!pick_mode(v, std))
        return false;
    return drmModeSetCrtc(v->fd, v->crtc_id, v->buf[v->back ^ 1].fb_id,
                          0, 0, &v->conn_id, 1, &v->mode) == 0;
}

pidvd_frame_t *pidvd_video_begin_frame(pidvd_video_t *v)
{
    struct kms_buf *b = &v->buf[v->back];
    v->frame.pixels = b->map;
    v->frame.width = v->mode.hdisplay;
    v->frame.height = v->mode.vdisplay;
    v->frame.stride = (int)b->pitch;
    return &v->frame;
}

static void flip_done(int fd, unsigned frame, unsigned sec, unsigned usec,
                      void *data)
{
    (void)fd; (void)frame; (void)sec; (void)usec;
    *(bool *)data = false;
}

bool pidvd_video_present(pidvd_video_t *v, pidvd_frame_t *f,
                         bool tff, bool rff)
{
    (void)f;
    /* TODO(field-sched): honor tff against mode field order; rff drives
     * 3:2 cadence on NTSC film content. PAL is 2 fields/frame so frame
     * flips at vsync are already field-accurate for this milestone. */
    (void)tff; (void)rff;

    if (drmModePageFlip(v->fd, v->crtc_id, v->buf[v->back].fb_id,
                        DRM_MODE_PAGE_FLIP_EVENT, &v->flip_pending) < 0)
        return false;
    v->flip_pending = true;

    drmEventContext ev = {
        .version = 2,
        .page_flip_handler = flip_done,
    };
    while (v->flip_pending) {
        struct pollfd p = { .fd = v->fd, .events = POLLIN };
        if (poll(&p, 1, 1000) <= 0)
            return false;
        drmHandleEvent(v->fd, &ev);
    }
    v->back ^= 1;
    return true;
}

void pidvd_video_close(pidvd_video_t *v)
{
    if (!v)
        return;
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
    }
    if (v->fd >= 0)
        close(v->fd);
    free(v);
}
