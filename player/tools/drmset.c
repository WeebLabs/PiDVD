/* drmset — atomically set a Composite mode + VEC TV-norm and show a test
 * grid, to prove (a) the atomic norm switch works where legacy setprop
 * fails with EINVAL, and (b) the CRT actually locks the requested
 * standard. Holds the mode ~30s then exits.
 *
 *   drmset <vdisplay> <NORM>     e.g. drmset 240 NTSC   drmset 576 PAL
 *
 * Dev/diagnostic tool, cross-compiled for the target. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

static uint32_t prop_id(int fd, uint32_t obj, uint32_t type, const char *name,
                        uint64_t *cur)
{
    drmModeObjectProperties *props = drmModeObjectGetProperties(fd, obj, type);
    uint32_t id = 0;
    if (props) {
        for (uint32_t i = 0; i < props->count_props && !id; i++) {
            drmModePropertyRes *p = drmModeGetProperty(fd, props->props[i]);
            if (p && !strcmp(p->name, name)) {
                id = p->prop_id;
                if (cur) *cur = props->prop_values[i];
            }
            if (p) drmModeFreeProperty(p);
        }
        drmModeFreeObjectProperties(props);
    }
    return id;
}

/* enum value for a named option of an enum property */
static int enum_val(int fd, uint32_t prop, const char *name, uint64_t *out)
{
    drmModePropertyRes *p = drmModeGetProperty(fd, prop);
    int ok = 0;
    if (p) {
        for (int e = 0; e < p->count_enums; e++)
            if (!strcmp(p->enums[e].name, name)) {
                *out = p->enums[e].value;
                ok = 1;
                break;
            }
        drmModeFreeProperty(p);
    }
    return ok;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: drmset <vdisplay> <NORM> [hold_secs]\n");
        return 2;
    }
    int want_v = atoi(argv[1]);
    const char *norm = argv[2];
    int hold = argc > 3 ? atoi(argv[3]) : 600;

    int fd = open("/dev/dri/card0", O_RDWR);
    if (fd < 0) { perror("open"); return 1; }
    if (drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1)) {
        perror("atomic cap"); return 1;
    }

    drmModeRes *r = drmModeGetResources(fd);
    drmModeConnector *conn = NULL;
    for (int i = 0; i < r->count_connectors; i++) {
        drmModeConnector *c = drmModeGetConnector(fd, r->connectors[i]);
        if (c && c->connector_type == DRM_MODE_CONNECTOR_Composite) {
            conn = c;
            break;
        }
        if (c) drmModeFreeConnector(c);
    }
    if (!conn) { fprintf(stderr, "no composite connector\n"); return 1; }

    drmModeModeInfo mode;
    int have = 0;
    for (int m = 0; m < conn->count_modes; m++)
        if (conn->modes[m].vdisplay == want_v) { mode = conn->modes[m]; have = 1; break; }
    if (!have) { fprintf(stderr, "no %d mode\n", want_v); return 1; }
    printf("mode %s %dx%d vref=%d flags=0x%x\n", mode.name, mode.hdisplay,
           mode.vdisplay, mode.vrefresh, mode.flags);

    /* crtc from the connector's encoder */
    drmModeEncoder *enc = drmModeGetEncoder(fd, conn->encoder_id);
    uint32_t crtc_id = enc ? enc->crtc_id : 0;
    if (!crtc_id) {
        for (int k = 0; k < r->count_crtcs && !crtc_id; k++) crtc_id = r->crtcs[k];
    }
    if (enc) drmModeFreeEncoder(enc);

    /* a plane that can drive this crtc */
    drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    drmModePlaneRes *pr = drmModeGetPlaneResources(fd);
    uint32_t plane_id = 0;
    int crtc_idx = 0;
    for (int k = 0; k < r->count_crtcs; k++) if (r->crtcs[k] == crtc_id) crtc_idx = k;
    for (uint32_t i = 0; i < pr->count_planes && !plane_id; i++) {
        drmModePlane *pl = drmModeGetPlane(fd, pr->planes[i]);
        if (pl && (pl->possible_crtcs & (1u << crtc_idx))) plane_id = pl->plane_id;
        if (pl) drmModeFreePlane(pl);
    }
    if (!plane_id) { fprintf(stderr, "no plane\n"); return 1; }

    /* dumb fb with a visible grid */
    struct drm_mode_create_dumb creq = { .width = mode.hdisplay,
                                         .height = mode.vdisplay, .bpp = 32 };
    ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq);
    uint32_t fb_id;
    drmModeAddFB(fd, mode.hdisplay, mode.vdisplay, 24, 32, creq.pitch,
                 creq.handle, &fb_id);
    struct drm_mode_map_dumb mreq = { .handle = creq.handle };
    ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq);
    uint8_t *map = mmap(NULL, creq.size, PROT_READ | PROT_WRITE, MAP_SHARED,
                        fd, mreq.offset);
    for (int y = 0; y < mode.vdisplay; y++) {
        uint32_t *row = (uint32_t *)(map + (size_t)y * creq.pitch);
        for (int x = 0; x < mode.hdisplay; x++) {
            int border = (x < 8 || x > mode.hdisplay - 9 ||
                          y < 4 || y > mode.vdisplay - 5);
            int grid = (x % 40 == 0) || (y % 20 == 0);
            row[x] = border ? 0xFFFFFFFF : grid ? 0x00FFB000 : 0x00200800;
        }
    }

    /* property ids */
    uint64_t cur_norm = 0, norm_val = 0;
    uint32_t p_con_crtc = prop_id(fd, conn->connector_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", NULL);
    uint32_t p_tvmode   = prop_id(fd, conn->connector_id, DRM_MODE_OBJECT_CONNECTOR, "TV mode", &cur_norm);
    uint32_t p_active   = prop_id(fd, crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE", NULL);
    uint32_t p_modeid   = prop_id(fd, crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID", NULL);
    uint32_t p_fb   = prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID", NULL);
    uint32_t p_pcrtc= prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID", NULL);
    uint32_t p_sx = prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_X", NULL);
    uint32_t p_sy = prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_Y", NULL);
    uint32_t p_sw = prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_W", NULL);
    uint32_t p_sh = prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_H", NULL);
    uint32_t p_cx = prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_X", NULL);
    uint32_t p_cy = prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_Y", NULL);
    uint32_t p_cw = prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_W", NULL);
    uint32_t p_ch = prop_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_H", NULL);
    if (!enum_val(fd, p_tvmode, norm, &norm_val)) {
        fprintf(stderr, "norm %s not found\n", norm); return 1;
    }
    printf("crtc=%u plane=%u TVmode cur=%llu -> %s=%llu\n", crtc_id, plane_id,
           (unsigned long long)cur_norm, norm, (unsigned long long)norm_val);

    uint32_t blob;
    drmModeCreatePropertyBlob(fd, &mode, sizeof(mode), &blob);

    /* Force a full VEC disable first, so the subsequent enable re-runs the
     * encoder's complete init (color subcarrier + timing). vc4 only fully
     * programs the VEC on enable, not on a live mode change — without this
     * off->on cycle a runtime switch leaves it half-programmed (distorted,
     * monochrome). */
    drmModeAtomicReq *off = drmModeAtomicAlloc();
    drmModeAtomicAddProperty(off, conn->connector_id, p_con_crtc, 0);
    drmModeAtomicAddProperty(off, crtc_id, p_active, 0);
    drmModeAtomicAddProperty(off, crtc_id, p_modeid, 0);
    drmModeAtomicAddProperty(off, plane_id, p_fb, 0);
    drmModeAtomicAddProperty(off, plane_id, p_pcrtc, 0);
    int rc_off = drmModeAtomicCommit(fd, off, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
    printf("atomic disable: %s\n", rc_off == 0 ? "OK" : strerror(errno));
    drmModeAtomicFree(off);
    usleep(120000);

    drmModeAtomicReq *req = drmModeAtomicAlloc();
    drmModeAtomicAddProperty(req, conn->connector_id, p_con_crtc, crtc_id);
    drmModeAtomicAddProperty(req, conn->connector_id, p_tvmode, norm_val);
    drmModeAtomicAddProperty(req, crtc_id, p_modeid, blob);
    drmModeAtomicAddProperty(req, crtc_id, p_active, 1);
    drmModeAtomicAddProperty(req, plane_id, p_fb, fb_id);
    drmModeAtomicAddProperty(req, plane_id, p_pcrtc, crtc_id);
    drmModeAtomicAddProperty(req, plane_id, p_sx, 0);
    drmModeAtomicAddProperty(req, plane_id, p_sy, 0);
    drmModeAtomicAddProperty(req, plane_id, p_sw, (uint64_t)mode.hdisplay << 16);
    drmModeAtomicAddProperty(req, plane_id, p_sh, (uint64_t)mode.vdisplay << 16);
    drmModeAtomicAddProperty(req, plane_id, p_cx, 0);
    drmModeAtomicAddProperty(req, plane_id, p_cy, 0);
    drmModeAtomicAddProperty(req, plane_id, p_cw, mode.hdisplay);
    drmModeAtomicAddProperty(req, plane_id, p_ch, mode.vdisplay);

    int rc = drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
    printf("atomic commit: %s\n", rc == 0 ? "OK" : strerror(errno));
    drmModeAtomicFree(req);
    if (rc) return 1;

    printf("holding %s %dp/%s for %ds — check the CRT\n", mode.name,
           want_v, norm, hold);
    fflush(stdout);
    sleep(hold);
    return 0;
}
