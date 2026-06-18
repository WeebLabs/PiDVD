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
    uint32_t plane_id;       /* CRTC primary plane (same one legacy flips use) */
    unsigned field_parity;   /* vblank-sequence parity flips latch on */
    unsigned anchor_par;     /* vblank-count parity sampled at the last modeset */
    unsigned hfilter;        /* menu composite horizontal low-pass: 0=off..3 */
    bool progressive;        /* 240p/288p menu mode (decimated scanout) */
    pidvd_standard_t std;    /* drives the VEC composite norm (NTSC/PAL) */
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

static bool composite_usable(const drmModeConnector *c)
{
    /* vc4's composite connector cannot hotplug-detect a CRT, so the kernel
     * reports UNKNOWN unless a video=...:e boot arg forced it on. Treat both
     * as usable; DISCONNECTED is the only hard no. */
    return c->connector_type == DRM_MODE_CONNECTOR_Composite
        && c->connection != DRM_MODE_DISCONNECTED;
}

static bool mode_matches(pidvd_standard_t std, bool progressive,
                         const drmModeModeInfo *mi)
{
    int want_v = progressive ? ((std == PIDVD_STD_PAL) ? 288 : 240)
                             : ((std == PIDVD_STD_PAL) ? 576 : 480);
    int want_htotal = (std == PIDVD_STD_PAL) ? 864 : 858;
    int want_vtotal = progressive ? ((std == PIDVD_STD_PAL) ? 312 : 262)
                                  : ((std == PIDVD_STD_PAL) ? 625 : 525);
    bool mi_int = (mi->flags & DRM_MODE_FLAG_INTERLACE) != 0;

    return mi_int == !progressive
        && mi->hdisplay == 720
        && mi->vdisplay == want_v
        && mi->clock == 13500
        && mi->htotal == want_htotal
        && mi->vtotal == want_vtotal;
}

static bool choose_crtc(pidvd_video_t *v, drmModeRes *res,
                        const drmModeConnector *c)
{
    drmModeEncoder *e = c->encoder_id
        ? drmModeGetEncoder(v->fd, c->encoder_id) : NULL;
    if (e && e->crtc_id) {
        v->crtc_id = e->crtc_id;
        for (int k = 0; k < res->count_crtcs; k++) {
            if (res->crtcs[k] == v->crtc_id) {
                v->crtc_index = k;
                drmModeFreeEncoder(e);
                return true;
            }
        }
    }
    if (e)
        drmModeFreeEncoder(e);

    for (int i = 0; i < c->count_encoders; i++) {
        e = drmModeGetEncoder(v->fd, c->encoders[i]);
        if (!e)
            continue;
        for (int k = 0; k < res->count_crtcs; k++) {
            if (e->possible_crtcs & (1u << k)) {
                v->crtc_id = res->crtcs[k];
                v->crtc_index = k;
                drmModeFreeEncoder(e);
                return true;
            }
        }
        drmModeFreeEncoder(e);
    }
    return false;
}

/* Find a Composite mode for the requested standard and scan. Interlaced:
 * 480i/576i. Progressive: 240p/288p (half the active lines, no interlace
 * flag). Sets v->mode/conn/crtc on success. */
static bool pick_mode(pidvd_video_t *v, pidvd_standard_t std, bool progressive)
{
    drmModeRes *res = drmModeGetResources(v->fd);
    if (!res)
        return false;
    bool found = false;
    for (int i = 0; i < res->count_connectors && !found; i++) {
        drmModeConnector *c = drmModeGetConnector(v->fd, res->connectors[i]);
        if (!c)
            continue;
        if (composite_usable(c)) {
            for (int m = 0; m < c->count_modes; m++) {
                drmModeModeInfo *mi = &c->modes[m];
                if (mode_matches(std, progressive, mi)) {
                    v->conn_id = c->connector_id;
                    if (!choose_crtc(v, res, c))
                        continue;
                    v->mode = *mi;
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

/* ---- atomic modeset --------------------------------------------------
 * The composite mode AND the VEC norm must be set together in an atomic
 * commit: a legacy drmModeSetCrtc (or a bare setprop) does NOT fully
 * program the VEC encoder, and switching modes that way leaves it
 * distorted with no chroma. Crucially the switch must be a full OFF then
 * ON cycle — that is what makes vc4's encoder atomic_enable re-run the
 * complete VEC init (config + colour subcarrier). Per-frame flips stay on
 * the legacy page-flip path (pidvd_video_present), which keeps the
 * field-parity interlace logic intact. */

static uint32_t prop_id(int fd, uint32_t obj, uint32_t type, const char *name)
{
    drmModeObjectProperties *props = drmModeObjectGetProperties(fd, obj, type);
    uint32_t id = 0;
    if (props) {
        for (uint32_t i = 0; i < props->count_props && !id; i++) {
            drmModePropertyRes *p = drmModeGetProperty(fd, props->props[i]);
            if (p && !strcmp(p->name, name))
                id = p->prop_id;
            if (p)
                drmModeFreeProperty(p);
        }
        drmModeFreeObjectProperties(props);
    }
    return id;
}

/* enum value of a named option on an enum property (e.g. TV mode -> PAL) */
static bool enum_val(int fd, uint32_t prop, const char *name, uint64_t *out)
{
    drmModePropertyRes *p = drmModeGetProperty(fd, prop);
    bool ok = false;
    if (p) {
        for (int e = 0; e < p->count_enums; e++)
            if (!strcmp(p->enums[e].name, name)) {
                *out = p->enums[e].value;
                ok = true;
                break;
            }
        drmModeFreeProperty(p);
    }
    return ok;
}

/* the CRTC's primary plane — the same plane legacy page-flips target */
static bool select_plane(pidvd_video_t *v)
{
    drmModePlaneRes *pr = drmModeGetPlaneResources(v->fd);
    if (!pr)
        return false;
    v->plane_id = 0;
    for (uint32_t i = 0; i < pr->count_planes && !v->plane_id; i++) {
        drmModePlane *pl = drmModeGetPlane(v->fd, pr->planes[i]);
        if (pl && (pl->possible_crtcs & (1u << v->crtc_index)))
            v->plane_id = pl->plane_id;
        if (pl)
            drmModeFreePlane(pl);
    }
    drmModeFreePlaneResources(pr);
    return v->plane_id != 0;
}

/* Full OFF->ON atomic modeset: mode + norm + plane, scanning out buf[0]. */
static bool commit_mode(pidvd_video_t *v)
{
    const char *norm = (v->std == PIDVD_STD_PAL) ? "PAL" : "NTSC";
    uint32_t cP = prop_id(v->fd, v->conn_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");
    uint32_t cTV = prop_id(v->fd, v->conn_id, DRM_MODE_OBJECT_CONNECTOR, "TV mode");
    uint32_t rA = prop_id(v->fd, v->crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE");
    uint32_t rM = prop_id(v->fd, v->crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID");
    uint32_t pF = prop_id(v->fd, v->plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID");
    uint32_t pC = prop_id(v->fd, v->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
    uint32_t pSX = prop_id(v->fd, v->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_X");
    uint32_t pSY = prop_id(v->fd, v->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_Y");
    uint32_t pSW = prop_id(v->fd, v->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_W");
    uint32_t pSH = prop_id(v->fd, v->plane_id, DRM_MODE_OBJECT_PLANE, "SRC_H");
    uint32_t pCX = prop_id(v->fd, v->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_X");
    uint32_t pCY = prop_id(v->fd, v->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_Y");
    uint32_t pCW = prop_id(v->fd, v->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_W");
    uint32_t pCH = prop_id(v->fd, v->plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_H");
    uint64_t norm_val = 0;
    if (!cP || !cTV || !rA || !rM || !pF || !pC || !pSW || !pSH || !pCW || !pCH
        || !enum_val(v->fd, cTV, norm, &norm_val)) {
        fprintf(stderr, "video: atomic props/norm '%s' unavailable\n", norm);
        return false;
    }

    /* OFF: tear the pipeline down so the ON commit is a full enable. */
    drmModeAtomicReq *off = drmModeAtomicAlloc();
    drmModeAtomicAddProperty(off, v->conn_id, cP, 0);
    drmModeAtomicAddProperty(off, v->crtc_id, rA, 0);
    drmModeAtomicAddProperty(off, v->crtc_id, rM, 0);
    drmModeAtomicAddProperty(off, v->plane_id, pF, 0);
    drmModeAtomicAddProperty(off, v->plane_id, pC, 0);
    int rc = drmModeAtomicCommit(v->fd, off, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
    drmModeAtomicFree(off);
    if (rc) {
        fprintf(stderr, "video: atomic OFF failed: %s\n", strerror(errno));
        return false;
    }
    usleep(120000);

    /* ON: program mode + norm + plane in one full enable. */
    uint32_t blob = 0;
    if (drmModeCreatePropertyBlob(v->fd, &v->mode, sizeof(v->mode), &blob)) {
        fprintf(stderr, "video: mode blob failed: %s\n", strerror(errno));
        return false;
    }
    drmModeAtomicReq *on = drmModeAtomicAlloc();
    drmModeAtomicAddProperty(on, v->conn_id, cP, v->crtc_id);
    drmModeAtomicAddProperty(on, v->conn_id, cTV, norm_val);
    drmModeAtomicAddProperty(on, v->crtc_id, rM, blob);
    drmModeAtomicAddProperty(on, v->crtc_id, rA, 1);
    drmModeAtomicAddProperty(on, v->plane_id, pF, v->buf[0].fb_id);
    drmModeAtomicAddProperty(on, v->plane_id, pC, v->crtc_id);
    drmModeAtomicAddProperty(on, v->plane_id, pSX, 0);
    drmModeAtomicAddProperty(on, v->plane_id, pSY, 0);
    drmModeAtomicAddProperty(on, v->plane_id, pSW, (uint64_t)v->mode.hdisplay << 16);
    drmModeAtomicAddProperty(on, v->plane_id, pSH, (uint64_t)v->mode.vdisplay << 16);
    drmModeAtomicAddProperty(on, v->plane_id, pCX, 0);
    drmModeAtomicAddProperty(on, v->plane_id, pCY, 0);
    drmModeAtomicAddProperty(on, v->plane_id, pCW, v->mode.hdisplay);
    drmModeAtomicAddProperty(on, v->plane_id, pCH, v->mode.vdisplay);
    rc = drmModeAtomicCommit(v->fd, on, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
    drmModeAtomicFree(on);
    drmModeDestroyPropertyBlob(v->fd, blob);
    if (rc) {
        fprintf(stderr, "video: atomic ON failed: %s\n", strerror(errno));
        return false;
    }
    fprintf(stderr, "video: VEC %s norm=%s (atomic off->on)\n",
            v->mode.name, norm);
    return true;
}

/* Allocate scanout buffers for the current mode (plus a full-height
 * scratch when progressive) and program the CRTC via an atomic off->on. */
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
    if (!select_plane(v)) {
        fprintf(stderr, "video: no plane for crtc\n");
        return false;
    }
    if (!commit_mode(v))
        return false;
    v->back = 1;
    return true;
}

/* Pick std/scan, with automatic fallback from progressive to interlaced
 * when the VEC doesn't expose a 240p/288p mode. Updates v->progressive. */
static bool select_mode(pidvd_video_t *v, pidvd_standard_t std,
                        pidvd_scan_t scan)
{
    v->std = std;
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

/* Auto-calibrate output field dominance after a modeset.
 *
 * On the VC4 VEC (Pi3, GEN_4) the physical field the PV starts on is a
 * per-modeset coin flip and there is NO readable field register. But the
 * cumulative DRM vblank-count parity at the first vblank after the modeset
 * was measured (18/18 plays, across reboots) to predict that dominance
 * exactly — the player already aligns flips to this counter via field_parity,
 * so it cannot pin the field but it CAN read which one it got and compensate.
 * Sample that parity and set field_parity = anchor_par ^ field_calib so the
 * weave always lands on the correct field. field_calib is the one-bit
 * per-install convention (default 1; PIDVD_KEY_FIELD flips it if a unit ever
 * comes up inverted). Progressive (menu) scanout has no field identity. */
/* Per-install field-dominance calibration bit: which sampled anchor parity
 * counts as 'correct'. Process-global (NOT per pidvd_video_t) so a
 * PIDVD_KEY_FIELD correction holds across plays — the player re-opens video
 * each play. Default 0 = measured correct on this board; PIDVD_FIELD_PARITY=1
 * inverts it for a unit that ever boots reversed. -1 = not yet read. */
static int g_field_calib = -1;

static void calibrate_field_parity(pidvd_video_t *v)
{
    if (g_field_calib < 0) {
        const char *e = getenv("PIDVD_FIELD_PARITY");
        g_field_calib = (e && e[0] == '1') ? 1 : 0;
    }
    if (v->progressive) {
        v->field_parity = 0;
        return;
    }
    uint32_t crtcbits = (uint32_t)v->crtc_index << DRM_VBLANK_HIGH_CRTC_SHIFT;
    drmVBlank w = { .request = { .type = DRM_VBLANK_RELATIVE | crtcbits,
                                 .sequence = 1 } };
    v->anchor_par = (drmWaitVBlank(v->fd, &w) == 0) ? (w.reply.sequence & 1u) : 0u;
    v->field_parity = v->anchor_par ^ (unsigned)g_field_calib;
    fprintf(stderr, "video: field auto-cal: anchor par=%u calib=%d -> "
            "field_parity=%u\n", v->anchor_par, g_field_calib, v->field_parity);
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
    /* Atomic is required to set the VEC mode+norm together; universal
     * planes exposes the primary plane the modeset and legacy flips share.
     * Neither disables the legacy drmModePageFlip/drmWaitVBlank path the
     * field-parity present loop relies on (vc4 is a full atomic driver). */
    if (drmSetClientCap(v->fd, DRM_CLIENT_CAP_ATOMIC, 1)
        || drmSetClientCap(v->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1)) {
        fprintf(stderr, "video: atomic/universal-planes cap unavailable: %s\n",
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
    /* Derive field_parity per-modeset from the sampled anchor parity and the
     * process-global calibration bit (PIDVD_FIELD_PARITY seeds it). */
    calibrate_field_parity(v);
    /* v->hfilter (menu composite low-pass) stays 0 until the picker pushes the
     * persisted SETTINGS value via pidvd_video_set_hfilter; only the
     * progressive menu path uses it, playback is left untouched. */
    fprintf(stderr, "video: %s %ux%u%s %.3f MHz htotal=%u vtotal=%u "
            "flags=0x%x on Composite-1, field parity %u\n",
            v->mode.name, v->mode.hdisplay, v->mode.vdisplay,
            v->progressive ? "p" : "i", v->mode.clock / 1000.0,
            v->mode.htotal, v->mode.vtotal, v->mode.flags,
            v->field_parity);
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
    calibrate_field_parity(v);   /* the new modeset re-rolled the field phase */
    fprintf(stderr, "video: switched to %s %ux%u%s %.3f MHz htotal=%u "
            "vtotal=%u flags=0x%x\n", v->mode.name, v->mode.hdisplay,
            v->mode.vdisplay, v->progressive ? "p" : "i",
            v->mode.clock / 1000.0, v->mode.htotal, v->mode.vtotal,
            v->mode.flags);
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

void pidvd_video_toggle_field_parity(pidvd_video_t *v)
{
    if (!v || v->progressive)
        return;   /* progressive scanout has no field identity to flip */
    /* Flip the 1-bit calibration and re-derive field_parity from this
     * modeset's sampled anchor parity. This both flips the current output
     * (presenter re-reads field_parity each present — one-frame latency, an
     * aligned unsigned store is atomic on A53) AND sticks for the session, so
     * subsequent plays / standard changes stay corrected via calibrate_*. */
    g_field_calib ^= 1;
    v->field_parity = v->anchor_par ^ (unsigned)g_field_calib;
    fprintf(stderr, "video: field calib -> %d (field_parity=%u, live)\n",
            g_field_calib, v->field_parity);
}

/* Horizontal low-pass for the composite MENU scanout. Sharp 2-4px UI text
 * carries luma energy above the NTSC/PAL colour subcarrier (~3.6/4.4 MHz),
 * which the TV's chroma decoder reads as false colour (cross-colour splotches
 * and edge fringing on text). Band-limit each row so that energy drops below
 * the subcarrier. Granular: level 0 = off, 1..8 from faint to strong, with
 * level 4 = the [1 2 1]/4 kernel. Symmetric integer kernels, edge-clamped;
 * XRGB8888 (B,G,R,X). The driving SETTINGS option lets the user pick the
 * lightest level that clears the splotching on their CRT. */
struct hfk { uint8_t radius, divisor; int8_t tap[7]; };
static const struct hfk HFK[9] = {
    { 0,  1, { 0 } },                      /* 0 off (handled before lookup) */
    { 1, 32, { 1, 30, 1 } },               /* 1 faintest */
    { 1, 16, { 1, 14, 1 } },               /* 2 */
    { 1,  8, { 1,  6, 1 } },               /* 3 (default) */
    { 1,  4, { 1,  2, 1 } },               /* 4 = the tested [1 2 1]/4 */
    { 1,  3, { 1,  1, 1 } },               /* 5 box-3 */
    { 2,  9, { 1, 2, 3, 2, 1 } },          /* 6 */
    { 2,  5, { 1, 1, 1, 1, 1 } },          /* 7 box-5 */
    { 3,  7, { 1, 1, 1, 1, 1, 1, 1 } },    /* 8 box-7 strongest */
};

static void hfilter_row(uint32_t *dst, const uint32_t *src, int w,
                        unsigned level)
{
    if (level == 0 || level > 8 || w < 3) {
        memcpy(dst, src, (size_t)w * 4);
        return;
    }
    const struct hfk *f = &HFK[level];
    int radius = f->radius, div = f->divisor;
    for (int x = 0; x < w; x++) {
        int b = 0, g = 0, r = 0;
        for (int t = -radius; t <= radius; t++) {
            int xi = x + t;
            xi = xi < 0 ? 0 : (xi >= w ? w - 1 : xi);
            uint32_t p = src[xi];
            int wt = f->tap[t + radius];
            b += wt * (int)(p & 0xff);
            g += wt * (int)((p >> 8) & 0xff);
            r += wt * (int)((p >> 16) & 0xff);
        }
        dst[x] = (uint32_t)(b / div) | ((uint32_t)(g / div) << 8)
               | ((uint32_t)(r / div) << 16);
    }
}

/* Set the menu's composite horizontal low-pass level (0=off..8). Driven by
 * the SETTINGS COMP FILTER option; takes effect on the picker's next present
 * (it re-presents every loop), so changes are live. */
void pidvd_video_set_hfilter(pidvd_video_t *v, unsigned level)
{
    if (!v)
        return;
    if (level > 8)
        level = 8;
    if (v->hfilter != level) {
        v->hfilter = level;
        fprintf(stderr, "video: menu comp-filter -> %u\n", level);
    }
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
            hfilter_row((uint32_t *)(b->map + (size_t)y * b->pitch),
                        (const uint32_t *)(v->scratch + (size_t)(2 * y) * bpl),
                        v->mode.hdisplay, v->hfilter);
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
