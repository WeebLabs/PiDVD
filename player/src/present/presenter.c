#include "present/presenter.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define RING_DEPTH 4

struct ring_frame {
    uint8_t *rgb;
    int width, height, stride;
    bool tff, rff;
};

struct pidvd_presenter {
    pidvd_video_t *video;
    struct ring_frame ring[RING_DEPTH];
    int head, tail, count;     /* guarded by lock */
    bool running;
    long frames;
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t not_full, not_empty;
};

static void *present_loop(void *opaque)
{
    pidvd_presenter_t *p = opaque;
    for (;;) {
        pthread_mutex_lock(&p->lock);
        while (p->count == 0 && p->running)
            pthread_cond_wait(&p->not_empty, &p->lock);
        if (p->count == 0 && !p->running) {
            pthread_mutex_unlock(&p->lock);
            return NULL;
        }
        struct ring_frame fr = p->ring[p->tail];
        pthread_mutex_unlock(&p->lock);

        pidvd_frame_t *f = pidvd_video_begin_frame(p->video);
        int rows = fr.height < f->height ? fr.height : f->height;
        int line = (fr.width < f->width ? fr.width : f->width) * 4;
        for (int y = 0; y < rows; y++)
            memcpy(f->pixels + (size_t)y * f->stride,
                   fr.rgb + (size_t)y * fr.stride, line);
        /* blocks until vsync flip completes — this is the clock */
        pidvd_video_present(p->video, f, fr.tff, fr.rff);

        pthread_mutex_lock(&p->lock);
        p->tail = (p->tail + 1) % RING_DEPTH;
        p->count--;
        p->frames++;
        pthread_cond_signal(&p->not_full);
        pthread_mutex_unlock(&p->lock);
    }
}

pidvd_presenter_t *pidvd_presenter_start(pidvd_video_t *video,
                                         int width, int height)
{
    pidvd_presenter_t *p = calloc(1, sizeof(*p));
    p->video = video;
    p->running = true;
    pthread_mutex_init(&p->lock, NULL);
    pthread_cond_init(&p->not_full, NULL);
    pthread_cond_init(&p->not_empty, NULL);
    for (int i = 0; i < RING_DEPTH; i++)
        p->ring[i].rgb = malloc((size_t)width * height * 4);
    if (pthread_create(&p->thread, NULL, present_loop, p) != 0) {
        free(p);
        return NULL;
    }
    return p;
}

void pidvd_presenter_push(pidvd_presenter_t *p, const uint8_t *rgb32,
                          int width, int height, int stride,
                          bool tff, bool rff)
{
    pthread_mutex_lock(&p->lock);
    while (p->count == RING_DEPTH)
        pthread_cond_wait(&p->not_full, &p->lock);
    struct ring_frame *fr = &p->ring[p->head];
    fr->width = width;
    fr->height = height;
    fr->stride = width * 4;
    fr->tff = tff;
    fr->rff = rff;
    for (int y = 0; y < height; y++)
        memcpy(fr->rgb + (size_t)y * fr->stride,
               rgb32 + (size_t)y * stride, (size_t)width * 4);
    p->head = (p->head + 1) % RING_DEPTH;
    p->count++;
    pthread_cond_signal(&p->not_empty);
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
    for (int i = 0; i < RING_DEPTH; i++)
        free(p->ring[i].rgb);
    free(p);
    return shown;
}
