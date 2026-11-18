// SPDX-License-Identifier: GPL-2.0-or-later
#include "platform.h"
#include <alsa/asoundlib.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define INPUT_SOCKET "/tmp/q2-boy.input.sock"
static int fb = -1, keys = -1, sock = -1, back, stride, original_y;
static struct fb_var_screeninfo video;
static size_t map_size;
static uint32_t *map;
static snd_pcm_t *pcm;
static int jack, soft_volume = 100, attenuation = 100;
static unsigned sample_rate = 65536;
volatile sig_atomic_t quitting;
int raw_buttons;

uint64_t clock_ns(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000u + now.tv_nsec;
}
static void request_quit(int sig) { (void)sig; quitting = 1; }

static void grab_buttons(void) {
    for (int i = 0; i < 32; ++i) {
        char path[48], name[80] = {0};
        snprintf(path, sizeof path, "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        if (ioctl(fd, EVIOCGNAME(sizeof name), name) >= 0 &&
            !strcmp(name, "md-gpio-keys") && ioctl(fd, EVIOCGRAB, 1) == 0) {
            keys = fd;
            raw_buttons = 1;
            return;
        }
        close(fd);
    }
}
static void dac(unsigned long request, int value) {
    int fd = open("/dev/shanling_dac", O_RDWR | O_CLOEXEC);
    if (fd >= 0) { ioctl(fd, request, &value); close(fd); }
}
static void open_audio(const char *device, const char *volume) {
    char usb[32] = "plughw:1,0";
    if (!device || !*device) device = "plughw:0,0";
    if (!strcmp(device, "usb")) {
        FILE *cards = fopen("/proc/asound/cards", "r");
        if (cards) {
            char row[256]; int n;
            while (fgets(row, sizeof row, cards)) {
                if (strstr(row, "USB-Audio") && sscanf(row, "%d", &n) == 1) {
                    snprintf(usb, sizeof usb, "plughw:%d,0", n);
                    break;
                }
            }
            fclose(cards);
        }
        device = usb;
    }
    soft_volume = 100;
    if (volume && *volume && *volume != 'h') {
        int n = atoi(volume);
        if (n >= 0 && n <= 100) soft_volume = n;
    }
    jack = !strcmp(device, "plughw:0,0");
    setenv("LIBASOUND_THREAD_SAFE", "0", 1);
    for (int i = 0; i < (jack ? 20 : 35) && !quitting; ++i) {
        if (snd_pcm_open(&pcm, device, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK) == 0) {
            if (snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                                   2, sample_rate, 1, 68000) == 0) {
                if (jack) { dac(0xc0044d1bu, 0); dac(0xc0044d1fu, 0); }
                return;
            }
            snd_pcm_close(pcm);
            pcm = NULL;
        }
        usleep(100000);
    }
    fprintf(stderr, "ALSA open failed for %s\n", device);
}
int platform_open(const char *device, const char *volume, int music) {
    signal(SIGTERM, request_quit);
    signal(SIGINT, request_quit);
    struct fb_fix_screeninfo fixed;
    fb = open("/dev/fb0", O_RDWR | O_CLOEXEC);
    if (fb < 0 || ioctl(fb, FBIOGET_VSCREENINFO, &video) ||
        ioctl(fb, FBIOGET_FSCREENINFO, &fixed)) return 0;
    original_y = video.yoffset;
    back = original_y ? 0 : WIDTH;
    stride = fixed.line_length / 4;
    if (video.xres != HEIGHT || video.yres != WIDTH || video.yres_virtual < 2 * WIDTH ||
        video.bits_per_pixel != 32 || video.red.offset != 16 || video.green.offset != 8 ||
        video.blue.offset != 0 || stride < HEIGHT ||
        (size_t)2 * WIDTH * stride * 4 > fixed.smem_len) return 0;
    map_size = fixed.smem_len;
    map = mmap(NULL, map_size, PROT_READ | PROT_WRITE, MAP_SHARED, fb, 0);
    if (map == MAP_FAILED) { map = NULL; return 0; }

    struct sockaddr_un address = { .sun_family = AF_UNIX, .sun_path = INPUT_SOCKET };
    sock = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (sock < 0) return 0;
    unlink(INPUT_SOCKET);
    if (bind(sock, (struct sockaddr *)&address, sizeof address)) return 0;
    if (!music) open_audio(device, volume);
    grab_buttons();
    fprintf(stderr, "ALSA=%s GPIO=%s\n", pcm ? "open" : "unavailable",
            raw_buttons ? "grabbed" : "using firmware fallback");
    return 1;
}
int platform_event(Event *e) {
    struct input_event key;
    if (keys >= 0 && read(keys, &key, sizeof key) == sizeof key) {
        if (key.type == EV_SYN && key.code == SYN_DROPPED) {
            *e = (Event){'r', 0, 0, 0};
        } else if (key.type == EV_KEY && key.value <= 1) {
            *e = (Event){'g', key.value ? 'd' : 'u', key.code, 0};
        } else *e = (Event){0};
        return 1;
    }
    char buffer[80];
    ssize_t n = sock < 0 ? -1 : recv(sock, buffer, sizeof buffer - 1, MSG_DONTWAIT);
    if (n < 1) return 0;
    buffer[n] = 0;
    *e = (Event){0};
    if (buffer[0] == 'w' && sscanf(buffer + 1, "%d", &e->x) == 1) e->kind = 'w';
    else if ((buffer[0] == 'k' || buffer[0] == 't') &&
             sscanf(buffer + 1, " %c %d %d", &e->action, &e->x, &e->y) >= 2)
        e->kind = buffer[0];
    return 1;
}
static void display_frame(const uint32_t *pixels) {
    uint32_t *page = map + back * stride;
    for (int x = 0; x < WIDTH; ++x)
        for (int y = 0; y < HEIGHT; ++y)
            page[x * stride + HEIGHT - 1 - y] = pixels[y * WIDTH + x];
    video.yoffset = back;
    if (!ioctl(fb, FBIOPAN_DISPLAY, &video)) back = back ? 0 : WIDTH;
}
/* The panel's page flip can block on scanout. Higher fast-forward targets
 * use a newest-frame mailbox so the emulation thread can keep going. */
static pthread_t display_thread;
static pthread_mutex_t display_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t display_ready = PTHREAD_COND_INITIALIZER;
static uint32_t display_buffers[2][WIDTH * HEIGHT];
static int fill_buffer, display_started, display_stop, display_pending, display_busy;

static void *display_loop(void *unused) {
    (void)unused;
    pthread_mutex_lock(&display_lock);
    while (!display_stop) {
        while (!display_pending && !display_stop) pthread_cond_wait(&display_ready, &display_lock);
        if (display_stop) break;
        const uint32_t *frame = display_buffers[fill_buffer];
        fill_buffer ^= 1;
        display_pending = 0;
        display_busy = 1;
        pthread_mutex_unlock(&display_lock);
        display_frame(frame);
        pthread_mutex_lock(&display_lock);
        display_busy = 0;
        pthread_cond_broadcast(&display_ready);
    }
    pthread_mutex_unlock(&display_lock);
    return NULL;
}
void platform_present(const uint32_t *pixels, int asynchronous) {
    if (asynchronous && !display_started && !display_stop) {
        if (!pthread_create(&display_thread, NULL, display_loop, NULL)) display_started = 1;
        else {
            display_stop = 1;
            fprintf(stderr, "Could not start display worker; using direct display updates\n");
        }
    }
    if (display_started) {
        pthread_mutex_lock(&display_lock);
        if (asynchronous) {
            memcpy(display_buffers[fill_buffer], pixels, sizeof display_buffers[0]);
            display_pending = 1;
            pthread_cond_signal(&display_ready);
            pthread_mutex_unlock(&display_lock);
            return;
        }
        display_pending = 0;
        while (display_busy) pthread_cond_wait(&display_ready, &display_lock);
        pthread_mutex_unlock(&display_lock);
    }
    display_frame(pixels);
}
static void stop_display(void) {
    if (!display_started) return;
    pthread_mutex_lock(&display_lock);
    display_stop = 1;
    pthread_cond_signal(&display_ready);
    pthread_mutex_unlock(&display_lock);
    pthread_join(display_thread, NULL);
}
void platform_yield_to_player(void) {
    if (display_started) {
        pthread_mutex_lock(&display_lock);
        display_pending = 0;
        while (display_busy) pthread_cond_wait(&display_ready, &display_lock);
        pthread_mutex_unlock(&display_lock);
    }
    if (keys >= 0) close(keys);
    keys = -1;
    raw_buttons = 0;
}
void platform_resume_from_player(void) {
    if (fb >= 0 && !ioctl(fb, FBIOGET_VSCREENINFO, &video)) back = video.yoffset ? 0 : WIDTH;
    if (keys < 0) grab_buttons();
}
void platform_audio_release(void) {
    if (!pcm) return;
    snd_pcm_drop(pcm);
    snd_pcm_close(pcm);
    pcm = NULL;
    if (jack) dac(0xc0044d1fu, 1);
}
void platform_audio_select(int route, int gain) {
    char volume[8];
    snprintf(volume, sizeof volume, "%d", gain >= 0 && gain <= 100 ? gain : 100);
    platform_audio_release();
    open_audio(route == 1 ? "plug:bluealsa" : route >= 2 ? "usb" : "plughw:0,0", volume);
}
void platform_audio_gain(int gain) {
    if (gain >= 0 && gain <= 100) soft_volume = gain;
}
void platform_audio_attenuation(int percent) {
    if (percent >= 0 && percent <= 100) attenuation = percent;
}
void platform_audio_set_rate(unsigned rate) {
    if (sample_rate == rate) return;
    sample_rate = rate;
    if (pcm) {
        snd_pcm_drop(pcm);
        if (snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                               2, sample_rate, 1, 68000) != 0) {
            fprintf(stderr, "Could not configure game audio at %u Hz\n", sample_rate);
            platform_audio_release();
        }
    }
}
void platform_audio_reset(void) {
    if (pcm) { snd_pcm_drop(pcm); snd_pcm_prepare(pcm); }
}
void platform_audio_write(const int16_t *samples, unsigned frames, int muted) {
    if (!pcm || muted) return;
    int volume = soft_volume * attenuation / 100;
    int16_t adjusted[1024 * 2];
    while (frames) {
        unsigned count = frames > 1024 ? 1024 : frames;
        const int16_t *data = samples;
        if (volume != 100) {
            for (unsigned i = 0; i < count * 2; ++i)
                adjusted[i] = samples[i] * volume / 100;
            data = adjusted;
        }
        snd_pcm_sframes_t written = snd_pcm_writei(pcm, data, count);
        if (written == -EPIPE || written == -ESTRPIPE) {
            if (snd_pcm_recover(pcm, written, 1) < 0) return;
            written = snd_pcm_writei(pcm, data, count);
        }
        if (written <= 0) return;
        samples += written * 2;
        frames -= written;
    }
}
void platform_close(void) {
    stop_display();
    if (keys >= 0) close(keys);
    platform_audio_release();
    if (sock >= 0) { close(sock); unlink(INPUT_SOCKET); }
    if (map) {
        video.yoffset = original_y;
        ioctl(fb, FBIOPAN_DISPLAY, &video);
        munmap(map, map_size);
    }
    if (fb >= 0) close(fb);
}
