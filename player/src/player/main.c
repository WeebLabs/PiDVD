/* pidvd-player — appliance entry point.
 *
 * Milestone 1 (current): identify the disc, modeset the VEC to the
 * disc's native interlaced standard, and play title 1's video stream
 * straight from the title VOBs (no nav/menus yet — that's milestone 2,
 * via libdvdnav, along with audio and the full field scheduler).
 */
#include <stdio.h>
#include <string.h>

#include <dvdread/dvd_reader.h>

#include "core/disc.h"
#include "decode/video_mpeg2.h"
#include "demux/ps.h"
#include "platform/platform.h"

#define SECTORS_PER_READ 64

struct play_ctx {
    pidvd_video_t *video;
    pidvd_vdec_t *vdec;
    pidvd_ps_t ps;
    long frames;
};

static void on_frame(void *opaque, const uint8_t *rgb32, int w, int h,
                     int stride, bool tff, bool rff)
{
    struct play_ctx *p = opaque;
    pidvd_frame_t *f = pidvd_video_begin_frame(p->video);
    int rows = h < f->height ? h : f->height;
    int line = (w < f->width ? w : f->width) * 4;
    for (int y = 0; y < rows; y++)
        memcpy(f->pixels + (size_t)y * f->stride,
               rgb32 + (size_t)y * stride, line);
    pidvd_video_present(p->video, f, tff, rff);
    p->frames++;
}

static void on_video_es(void *opaque, const uint8_t *data, size_t len)
{
    struct play_ctx *p = opaque;
    pidvd_vdec_push(p->vdec, data, len);
}

static int play_title_vobs(pidvd_disc_t *d, int vts_nr,
                           pidvd_standard_t std)
{
    struct play_ctx p = { 0 };

    p.video = pidvd_video_open(std);
    if (!p.video)
        return 1;
    p.vdec = pidvd_vdec_new(on_frame, &p);
    pidvd_ps_init(&p.ps, on_video_es, &p);

    dvd_file_t *vobs = DVDOpenFile(pidvd_disc_reader(d), vts_nr,
                                   DVD_READ_TITLE_VOBS);
    if (!vobs) {
        fprintf(stderr, "pidvd: cannot open VTS %d title VOBs\n", vts_nr);
        pidvd_vdec_free(p.vdec);
        pidvd_video_close(p.video);
        return 1;
    }

    static unsigned char buf[2048 * SECTORS_PER_READ];
    int offset = 0;
    ssize_t got;
    while ((got = DVDReadBlocks(vobs, offset, SECTORS_PER_READ, buf)) > 0) {
        for (ssize_t s = 0; s < got; s++)
            pidvd_ps_push_sector(&p.ps, buf + s * 2048);
        offset += (int)got;
    }

    fprintf(stderr, "pidvd: end of title VOBs, %ld frames shown\n",
            p.frames);
    DVDCloseFile(vobs);
    pidvd_vdec_free(p.vdec);
    pidvd_video_close(p.video);
    return 0;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : NULL;
    if (!path) {
        fprintf(stderr, "pidvd-player: no disc given\n");
        return 2;
    }

    pidvd_disc_t *d = pidvd_disc_open(path);
    if (!d)
        return 1;

    bool mixed;
    pidvd_standard_t std = pidvd_disc_standard(d, &mixed);
    printf("pidvd: disc '%s', %d titles, output %s%s\n",
           pidvd_disc_volume_id(d), pidvd_disc_title_count(d),
           pidvd_standard_name(std), mixed ? " (mixed-standard disc)" : "");

    const pidvd_title_t *t1 = pidvd_disc_title(d, 0);
    int rc = play_title_vobs(d, t1 ? t1->vts_nr : 1, std);

    pidvd_disc_close(d);
    return rc;
}
