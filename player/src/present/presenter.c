#define _GNU_SOURCE   /* CPU_SET / pthread affinity */
#include "present/presenter.h"

#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>

#ifdef __ARM_NEON
#include <arm_neon.h>
#endif

#define RING_DEPTH 8

struct ring_frame {
    uint8_t *y, *u, *v;
    int width, height;
    bool tff, rff;
    int64_t pts;
    uint64_t epoch;
};

struct pidvd_presenter {
    pidvd_video_t *video;
    pidvd_blend_cb blend;
    void *blend_ctx;
    pidvd_prepare_cb prepare;
    pidvd_presented_cb presented;
    void *timing_ctx;
    uint64_t displayed_epoch;
    bool have_displayed_epoch;
    struct ring_frame ring[RING_DEPTH];
    int max_w, max_h;
    int head, tail, count;     /* guarded by lock */
    bool busy;
    bool running;
    long frames;
    uint8_t *rgb;              /* cached convert+blend buffer */
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t not_full, not_empty, idle;
};

static inline uint8_t clamp8(int v)
{
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}

#ifdef __ARM_NEON
/* BT.601 video range, 6-bit coefficients: R=(75C+102E)>>6,
 * G=(75C-25D-52E)>>6, B=(75C+129D)>>6 with C=y-16,D=cb-128,E=cr-128.
 * 16 pixels per iteration, interleaved BGRX store. */
static void yuv_row_neon(const uint8_t *py, const uint8_t *pu,
                         const uint8_t *pv, uint8_t *d, int width)
{
    for (int x = 0; x + 16 <= width; x += 16) {
        uint8x16_t yv = vld1q_u8(py + x);
        uint8x8_t u8 = vld1_u8(pu + x / 2);
        uint8x8_t v8 = vld1_u8(pv + x / 2);
        uint8x8x2_t uz = vzip_u8(u8, u8);   /* duplicate chroma */
        uint8x8x2_t vz = vzip_u8(v8, v8);

        for (int half = 0; half < 2; half++) {
            int16x8_t C = vreinterpretq_s16_u16(vmovl_u8(
                half ? vget_high_u8(yv) : vget_low_u8(yv)));
            int16x8_t D = vreinterpretq_s16_u16(vmovl_u8(uz.val[half]));
            int16x8_t E = vreinterpretq_s16_u16(vmovl_u8(vz.val[half]));
            C = vmulq_n_s16(vsubq_s16(C, vdupq_n_s16(16)), 75);
            D = vsubq_s16(D, vdupq_n_s16(128));
            E = vsubq_s16(E, vdupq_n_s16(128));

            int16x8_t R = vaddq_s16(C, vmulq_n_s16(E, 102));
            int16x8_t G = vsubq_s16(vsubq_s16(C, vmulq_n_s16(D, 25)),
                                    vmulq_n_s16(E, 52));
            int16x8_t B = vaddq_s16(C, vmulq_n_s16(D, 129));

            uint8x8x4_t out;
            out.val[0] = vqshrun_n_s16(B, 6);
            out.val[1] = vqshrun_n_s16(G, 6);
            out.val[2] = vqshrun_n_s16(R, 6);
            out.val[3] = vdup_n_u8(0);
            vst4_u8(d + (size_t)(x + half * 8) * 4, out);
        }
    }
}
#endif

/* BT.601 video-range planar 4:2:0 -> XRGB (bytes B,G,R,X). Two luma
 * rows per chroma row. */
static void yuv_to_rgb(const struct ring_frame *f, uint8_t *dst)
{
    int cw = f->width / 2;
    for (int y = 0; y < f->height; y++) {
        const uint8_t *py = f->y + (size_t)y * f->width;
        const uint8_t *pu = f->u + (size_t)(y / 2) * cw;
        const uint8_t *pv = f->v + (size_t)(y / 2) * cw;
        uint8_t *d = dst + (size_t)y * f->width * 4;
#ifdef __ARM_NEON
        if ((f->width & 15) == 0) {
            yuv_row_neon(py, pu, pv, d, f->width);
            continue;
        }
#endif
        for (int x = 0; x < f->width; x += 2) {
            int cb = pu[x / 2] - 128;
            int cr = pv[x / 2] - 128;
            int rr = 409 * cr + 128;
            int gg = -100 * cb - 208 * cr + 128;
            int bb = 516 * cb + 128;
            for (int k = 0; k < 2; k++) {
                int yy = 298 * (py[x + k] - 16);
                d[0] = clamp8((yy + bb) >> 8);
                d[1] = clamp8((yy + gg) >> 8);
                d[2] = clamp8((yy + rr) >> 8);
                d[3] = 0;
                d += 4;
            }
        }
    }
}

static void *present_loop(void *opaque)
{
    pidvd_presenter_t *p = opaque;

    /* own core + realtime priority: the presenter must hit its field
     * slot every frame; scheduler jitter = a visible repeated frame */
    cpu_set_t cpus;
    CPU_ZERO(&cpus);
    CPU_SET(3, &cpus);
    pthread_setaffinity_np(pthread_self(), sizeof(cpus), &cpus);
    struct sched_param sp = { .sched_priority = 40 };
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);

    for (;;) {
        pthread_mutex_lock(&p->lock);
        while (p->count == 0 && p->running)
            pthread_cond_wait(&p->not_empty, &p->lock);
        if (p->count == 0 && !p->running) {
            pthread_mutex_unlock(&p->lock);
            return NULL;
        }
        struct ring_frame *fr = &p->ring[p->tail];
        p->busy = true;
        pthread_mutex_unlock(&p->lock);

        if ((!p->have_displayed_epoch || fr->epoch != p->displayed_epoch)
            && p->prepare)
            p->prepare(p->timing_ctx, fr->epoch);

        yuv_to_rgb(fr, p->rgb);
        if (p->blend)
            p->blend(p->blend_ctx, p->rgb, fr->width, fr->height);

        pidvd_frame_t *f = pidvd_video_begin_frame(p->video);
        int rows = fr->height < f->height ? fr->height : f->height;
        int line = (fr->width < f->width ? fr->width : f->width) * 4;
        for (int y = 0; y < rows; y++)
            memcpy(f->pixels + (size_t)y * f->stride,
                   p->rgb + (size_t)y * fr->width * 4, line);
        pidvd_video_stamp_t stamp;
        bool shown = pidvd_video_present(p->video, f, fr->tff, fr->rff,
                                         &stamp);
        if (shown && p->presented)
            p->presented(p->timing_ctx, fr->epoch, fr->pts, &stamp);

        pthread_mutex_lock(&p->lock);
        p->busy = false;
        pthread_cond_broadcast(&p->idle);
        if (shown) {
            p->displayed_epoch = fr->epoch;
            p->have_displayed_epoch = true;
            p->tail = (p->tail + 1) % RING_DEPTH;
            p->count--;
            p->frames++;
            pthread_cond_signal(&p->not_full);
        }
        pthread_mutex_unlock(&p->lock);
    }
}

pidvd_presenter_t *pidvd_presenter_start(pidvd_video_t *video,
                                         int width, int height,
                                         pidvd_blend_cb blend, void *ctx,
                                         pidvd_prepare_cb prepare,
                                         pidvd_presented_cb presented,
                                         void *timing_ctx)
{
    pidvd_presenter_t *p = calloc(1, sizeof(*p));
    p->video = video;
    p->blend = blend;
    p->blend_ctx = ctx;
    p->prepare = prepare;
    p->presented = presented;
    p->timing_ctx = timing_ctx;
    p->max_w = width;
    p->max_h = height;
    p->running = true;
    pthread_mutex_init(&p->lock, NULL);
    pthread_cond_init(&p->not_full, NULL);
    pthread_cond_init(&p->not_empty, NULL);
    pthread_cond_init(&p->idle, NULL);
    size_t luma = (size_t)width * height;
    for (int i = 0; i < RING_DEPTH; i++) {
        p->ring[i].y = malloc(luma);
        p->ring[i].u = malloc(luma / 4);
        p->ring[i].v = malloc(luma / 4);
    }
    p->rgb = malloc(luma * 4);
    if (pthread_create(&p->thread, NULL, present_loop, p) != 0) {
        free(p);
        return NULL;
    }
    return p;
}

void pidvd_presenter_push(pidvd_presenter_t *p,
                          const uint8_t *y, const uint8_t *u,
                          const uint8_t *v, int width, int height,
                          bool tff, bool rff, int64_t pts, uint64_t epoch)
{
    if (width > p->max_w) width = p->max_w;
    if (height > p->max_h) height = p->max_h;
    pthread_mutex_lock(&p->lock);
    while (p->count == RING_DEPTH)
        pthread_cond_wait(&p->not_full, &p->lock);
    struct ring_frame *fr = &p->ring[p->head];
    fr->width = width;
    fr->height = height;
    fr->tff = tff;
    fr->rff = rff;
    fr->pts = pts;
    fr->epoch = epoch;
    memcpy(fr->y, y, (size_t)width * height);
    memcpy(fr->u, u, (size_t)width * height / 4);
    memcpy(fr->v, v, (size_t)width * height / 4);
    p->head = (p->head + 1) % RING_DEPTH;
    p->count++;
    pthread_cond_signal(&p->not_empty);
    pthread_mutex_unlock(&p->lock);
}

void pidvd_presenter_reset(pidvd_presenter_t *p)
{
    pthread_mutex_lock(&p->lock);
    while (p->busy)
        pthread_cond_wait(&p->idle, &p->lock);
    p->head = p->tail = p->count = 0;
    pthread_cond_broadcast(&p->not_full);
    pthread_mutex_unlock(&p->lock);
}

long pidvd_presenter_stop(pidvd_presenter_t *p)
{
    pthread_mutex_lock(&p->lock);
    p->running = false;
    pthread_cond_broadcast(&p->not_empty);
    pthread_mutex_unlock(&p->lock);
    pthread_join(p->thread, NULL);
    long shown = p->frames;
    for (int i = 0; i < RING_DEPTH; i++) {
        free(p->ring[i].y);
        free(p->ring[i].u);
        free(p->ring[i].v);
    }
    free(p->rgb);
    free(p);
    return shown;
}
