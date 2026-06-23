/* evdev input: USB keyboards and HID media remotes normalize to
 * pidvd_key_t. All /dev/input/event* devices are polled together. */
#include "platform/platform.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_DEVS 16
#define CTL_FIFO "/tmp/pidvd-ctl"

struct pidvd_input {
    int fd[MAX_DEVS];
    int n;
    int ctl;               /* named-pipe remote control */
};

pidvd_input_t *pidvd_input_open(void)
{
    pidvd_input_t *in = calloc(1, sizeof(*in));
    for (int i = 0; i < 32 && in->n < MAX_DEVS; i++) {
        char path[40];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd >= 0)
            in->fd[in->n++] = fd;
    }
    unlink(CTL_FIFO);
    mkfifo(CTL_FIFO, 0666);
    in->ctl = open(CTL_FIFO, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    fprintf(stderr, "input: %d evdev device(s), control fifo %s\n",
            in->n, CTL_FIFO);
    return in;
}

static pidvd_key_t map_word(const char *w)
{
    static const struct { const char *w; pidvd_key_t k; } tab[] = {
        { "up", PIDVD_KEY_UP }, { "down", PIDVD_KEY_DOWN },
        { "left", PIDVD_KEY_LEFT }, { "right", PIDVD_KEY_RIGHT },
        { "enter", PIDVD_KEY_ENTER }, { "menu", PIDVD_KEY_MENU },
        { "title", PIDVD_KEY_TITLE }, { "pause", PIDVD_KEY_PLAY_PAUSE },
        { "stop", PIDVD_KEY_STOP }, { "next", PIDVD_KEY_NEXT_CHAPTER },
        { "prev", PIDVD_KEY_PREV_CHAPTER }, { "audio", PIDVD_KEY_AUDIO },
        { "sub", PIDVD_KEY_SUBTITLE }, { "field", PIDVD_KEY_FIELD },
        { "volup", PIDVD_KEY_VOL_UP }, { "voldown", PIDVD_KEY_VOL_DOWN },
    };
    for (unsigned i = 0; i < sizeof(tab) / sizeof(tab[0]); i++)
        if (!strcmp(w, tab[i].w))
            return tab[i].k;
    return PIDVD_KEY_NONE;
}

static pidvd_key_t map_key(unsigned code)
{
    switch (code) {
    case KEY_UP:        return PIDVD_KEY_UP;
    case KEY_DOWN:      return PIDVD_KEY_DOWN;
    case KEY_LEFT:      return PIDVD_KEY_LEFT;
    case KEY_RIGHT:     return PIDVD_KEY_RIGHT;
    case KEY_ENTER:
    case KEY_KPENTER:
    case KEY_OK:        return PIDVD_KEY_ENTER;
    case KEY_ESC:
    case KEY_M:
    case KEY_MENU:      return PIDVD_KEY_MENU;
    case KEY_T:
    case KEY_TITLE:     return PIDVD_KEY_TITLE;
    case KEY_SPACE:
    case KEY_PLAYPAUSE: return PIDVD_KEY_PLAY_PAUSE;
    case KEY_S:
    case KEY_STOP:      return PIDVD_KEY_STOP;
    case KEY_N:
    case KEY_PAGEDOWN:
    case KEY_NEXTSONG:  return PIDVD_KEY_NEXT_CHAPTER;
    case KEY_P:
    case KEY_PAGEUP:
    case KEY_PREVIOUSSONG: return PIDVD_KEY_PREV_CHAPTER;
    case KEY_A:         return PIDVD_KEY_AUDIO;
    case KEY_C:         return PIDVD_KEY_SUBTITLE;
    case KEY_F:         return PIDVD_KEY_FIELD;
    case KEY_VOLUMEUP:    return PIDVD_KEY_VOL_UP;
    case KEY_VOLUMEDOWN:  return PIDVD_KEY_VOL_DOWN;
    default:            return PIDVD_KEY_NONE;
    }
}

pidvd_key_t pidvd_input_poll(pidvd_input_t *in)
{
    if (in->ctl >= 0) {
        char line[32];
        ssize_t r = read(in->ctl, line, sizeof(line) - 1);
        if (r > 0) {
            while (r > 0 && (line[r - 1] == '\n' || line[r - 1] == '\r'))
                r--;
            line[r] = 0;
            pidvd_key_t k = map_word(line);
            if (k != PIDVD_KEY_NONE)
                return k;
        }
    }

    struct input_event ev;
    for (int i = 0; i < in->n; i++) {
        while (read(in->fd[i], &ev, sizeof(ev)) == sizeof(ev)) {
            if (ev.type == EV_KEY && ev.value == 1) {
                pidvd_key_t k = map_key(ev.code);
                if (k != PIDVD_KEY_NONE)
                    return k;
            }
        }
    }
    return PIDVD_KEY_NONE;
}

void pidvd_input_close(pidvd_input_t *in)
{
    if (!in)
        return;
    for (int i = 0; i < in->n; i++)
        close(in->fd[i]);
    if (in->ctl >= 0)
        close(in->ctl);
    free(in);
}
