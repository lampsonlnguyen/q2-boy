// SPDX-License-Identifier: GPL-2.0-or-later
#include "platform.h"
#include "libretro.h"
#include <dlfcn.h>
#include "font.h"
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define CART_WIDTH 240
#define CART_HEIGHT 160
#define SLOTS 10
#define ROWS 5
#define ROM_LIMIT 200
#define KEY(id) (1u << RETRO_DEVICE_ID_JOYPAD_##id)
#define RGB(value) (0xff000000u | (value))
#define ACCENT RGB(0x87e4c7)
#define WHITE RGB(0xf7faf9)
#define GRAY RGB(0xa6b9bb)
#define DARK RGB(0x0c161a)
#define PATH_CAP 700

enum Screen { BROWSER, GAME, MENU, STATES, SETTINGS, EDIT, CONFIRM, AUDIO, PHYSICAL, TOUCH, ASSIGN };
enum Scale { FIT, CROP, STRETCH };
enum Control { L, R, B, MENU_BUTTON, SPEED, QUICK_SAVE, QUICK_LOAD, AB, SELECT_BUTTON, START_BUTTON, TOUCH_A, TOUCH_UP, TOUCH_DOWN, TOUCH_LEFT, TOUCH_RIGHT, CONTROLS };
enum Action { SAVE = 1, LOAD };
typedef struct { int x, y, size, opacity, visible; unsigned keys; const char *label; } Button;

#define EDGE_INSET 12
static const Button defaults[CONTROLS] = {
    {18, 16, 72, 58, 1, KEY(L), "L"}, {285, 16, 72, 58, 1, KEY(R), "R"},
    {297, 244, 60, 58, 1, KEY(B), "B"},
    {18, 260, 62, 65, 1, 0, "Menu"}, {88, 260, 44, 65, 0, 0, "FF"},
    {122, 208, 44, 65, 0, 0, "Save"}, {174, 208, 44, 65, 0, 0, "Load"},
    {310, 192, 46, 58, 0, KEY(A) | KEY(B), "A+B"},
    {126, 268, 56, 65, 1, KEY(SELECT), "Select"},
    {192, 268, 56, 65, 1, KEY(START), "Start"},
    {253, 244, 40, 62, 0, KEY(A), "A"},
    {64, 127, 42, 62, 0, KEY(UP), "Up"},
    {64, 215, 42, 62, 0, KEY(DOWN), "Down"},
    {20, 171, 42, 62, 0, KEY(LEFT), "Left"},
    {108, 171, 42, 62, 0, KEY(RIGHT), "Right"}
};
enum Physical { P_RETURN, P_PLAY, P_PREVIOUS, P_NEXT, P_CENTRE, PHYSICAL_COUNT };
static const char *const physical_names[] = {"Return", "Play/Pause", "Previous", "Next", "Centre"};
static const unsigned physical_defaults[] = {KEY(UP), KEY(DOWN), KEY(LEFT), KEY(RIGHT), KEY(A)};
static unsigned physical_map[PHYSICAL_COUNT], held_buttons, gameplay_buttons;
static const struct { unsigned keys; const char *name; } actions[] = {
    {KEY(A), "A"}, {KEY(B), "B"}, {KEY(START), "Start"}, {KEY(SELECT), "Select"},
    {KEY(L), "L"}, {KEY(R), "R"}, {KEY(UP), "Up"}, {KEY(DOWN), "Down"},
    {KEY(LEFT), "Left"}, {KEY(RIGHT), "Right"}, {KEY(A) | KEY(B), "A+B"}, {0, "Unassigned"}
};
static const int touch_choices[] = {TOUCH_A, B, SELECT_BUTTON, START_BUTTON, L, R,
    TOUCH_UP, TOUCH_DOWN, TOUCH_LEFT, TOUCH_RIGHT, AB, SPEED, QUICK_SAVE, QUICK_LOAD};
static int touch_page, assign_page, assign_target;
static enum Screen assign_return;
static const char *action_name(unsigned keys) {
    for (unsigned i = 0; i < sizeof actions / sizeof actions[0]; ++i)
        if (actions[i].keys == keys) return actions[i].name;
    return "Unassigned";
}
static bool valid_action(unsigned keys) {
    for (unsigned i = 0; i < sizeof actions / sizeof actions[0]; ++i)
        if (actions[i].keys == keys) return true;
    return false;
}
static bool remappable_overlay(int which) { return which < MENU_BUTTON || which >= AB; }
static const char *button_label(int which, const Button *b) {
    return remappable_overlay(which) ? action_name(b->keys) : b->label;
}
static const int legacy_geometry[][3] = {
    {6, 6, 42}, {327, 6, 42}, {265, 264, 48}, {321, 264, 48}, {6, 264, 48},
    {66, 264, 44}, {122, 264, 44}, {176, 264, 44}, {320, 206, 46}
};
static const int previous_overlays[] = {L, R, -1, B, MENU_BUTTON, SPEED, QUICK_SAVE, QUICK_LOAD, AB};
static const int speeds[] = {2, 3};
static int fast_rate = 2;
#define AUDIO_REQUEST "/tmp/q2-boy.audio.req"
#define AUDIO_REPLY "/tmp/q2-boy.audio.reply"
#define AUDIO_MAGIC 0x51474131u
enum AudioAction { AUDIO_GAME = 1, AUDIO_MUSIC, AUDIO_VOLUME, AUDIO_PREVIOUS, AUDIO_PLAY, AUDIO_NEXT, AUDIO_NOW_PLAYING };
struct AudioCommand { uint32_t magic, seq; int32_t action, value; };
struct AudioReply { uint32_t magic, seq; int32_t music, volume, limit, route, gain, playing, native_ui; };
static struct AudioReply audio_reply;
static bool audio_bridge, music_mode, native_supported, native_yielded;
static int local_volume = 100, audio_route;
static uint32_t audio_seq;
static enum AudioAction pending_action;
static uint64_t audio_polled, audio_request_time;
static bool core_options_changed, allow_video = true;
static uint64_t next_video;
#define FAST_DISPLAY_NS 33333333ull
static double measured_speed;
static Button buttons[CONTROLS];
static enum Screen screen = BROWSER, confirm_from;
static enum Scale scaling;
static int running, fast, dirty = 1, selection, slot, edit_button, dragging = -1;
static int drag_x, drag_y, edit_start_x, edit_start_y, edit_moved, touch_button = -1, wheel_delta, menu_focus;

static enum Action confirm_action;
static uint64_t wheel_time, notice_end, next_save;
static unsigned physical_keys, touch_keys, pressed_this_frame;
static char notice[95];
static const char *data_dir = "/mnt/mmc/q2-boy";
static char rom_dir[PATH_CAP], save_dir[PATH_CAP], game_base[PATH_CAP], config_file[PATH_CAP];
static char roms[ROM_LIMIT][256];
static int rom_count;
static uint32_t cart[CART_WIDTH * CART_HEIGHT], canvas[WIDTH * HEIGHT], color[65536];
static bool smoke, have_frame, use_dynarec;
static volatile sig_atomic_t launch_stage;
static uint64_t speed_start;
static unsigned speed_frames, video_frames;
static double fps = 59.7275;

/* gpSP remains built in. Load the GB/GBC core only when a cartridge needs it. */
#define CORE_API(X) \
    X(set_environment) X(set_video_refresh) X(set_audio_sample) X(set_audio_sample_batch) \
    X(set_input_poll) X(set_input_state) X(init) X(deinit) X(set_controller_port_device) \
    X(load_game) X(unload_game) X(get_system_av_info) X(run) X(get_memory_size) \
    X(get_memory_data) X(serialize_size) X(serialize) X(unserialize)
struct Core {
#define API_MEMBER(name) __typeof__(&retro_##name) name;
    CORE_API(API_MEMBER)
#undef API_MEMBER
};
static const struct Core gpsp = {
#define API_GPSP(name) .name = retro_##name,
    CORE_API(API_GPSP)
#undef API_GPSP
};
static struct Core gambatte;
static const struct Core *core;
static void *gambatte_library;
static char app_dir[PATH_CAP];
static unsigned cart_width = CART_WIDTH, cart_height = CART_HEIGHT;
static unsigned smoke_audio_frames;

enum RomType { ROM_UNKNOWN, ROM_GBA, ROM_GB };
static enum RomType rom_type(const char *path) {
    const char *extension = strrchr(path, '.');
    if (!extension) return ROM_UNKNOWN;
    if (!strcasecmp(extension, ".gba")) return ROM_GBA;
    if (!strcasecmp(extension, ".gb") || !strcasecmp(extension, ".gbc")) return ROM_GB;
    return ROM_UNKNOWN;
}
static bool enable_gambatte(void) {
    if (gambatte_library) return true;
    char path[PATH_CAP];
    int length = snprintf(path, sizeof path, "%s/gambatte_libretro.so", app_dir);
    if (length < 0 || length >= (int)sizeof path) return false;
    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        fprintf(stderr, "Gambatte unavailable: %s\n", dlerror());
        return false;
    }
#define API_LOAD(name) \
    do { \
        gambatte.name = (__typeof__(gambatte.name))dlsym(handle, "retro_" #name); \
        if (!gambatte.name) { fprintf(stderr, "Gambatte lacks retro_%s\n", #name); dlclose(handle); return false; } \
    } while (0);
    CORE_API(API_LOAD)
#undef API_LOAD
    gambatte_library = handle;
    return true;
}
static void initialize_core(const struct Core *selected);
static void release_core(void) {
    if (core) { core->deinit(); core = NULL; }
}
static void locate_app(const char *argv0) {
    ssize_t n = readlink("/proc/self/exe", app_dir, sizeof app_dir - 1);
    if (n > 0) app_dir[n] = 0;
    else snprintf(app_dir, sizeof app_dir, "%s", argv0);
    char *slash = strrchr(app_dir, '/');
    if (slash == app_dir) slash[1] = 0;
    else if (slash) *slash = 0;
    else strcpy(app_dir, ".");
}
static int clamp(int x, int low, int high) { return x < low ? low : x > high ? high : x; }
/* 2x has headroom with the original path. At higher targets pace video by
 * elapsed time, leaving extra GBA frames to advance without a panel update. */
static bool paced_video(void) { return screen == GAME && fast && (fast_rate >= 3 || core == &gambatte); }
static void schedule_video(uint64_t now) {
    allow_video = !paced_video() || !next_video || now >= next_video;
    if (paced_video() && allow_video) {
        if (!next_video || now - next_video >= FAST_DISPLAY_NS) next_video = now;
        next_video += FAST_DISPLAY_NS;
    }
}
static int button_height(int which, const Button *b) {
    if (which == MENU_BUTTON) return (b->size * 7 + 5) / 10;
    if (which == L || which == R) return (b->size * 5 + 4) / 9;
    if (which == SELECT_BUTTON || which == START_BUTTON) return (b->size * 3 + 2) / 5;
    return b->size;
}
static int button_radius(int which, const Button *b) {
    return which == B ? b->size / 2 : 8;
}
static void move_button(int which, int x, int y) {
    Button *b = &buttons[which];
    b->x = clamp(x, EDGE_INSET, WIDTH - EDGE_INSET - b->size);
    b->y = clamp(y, EDGE_INSET, HEIGHT - EDGE_INSET - button_height(which, b));
}
static bool inside_shape(int x, int y, int w, int h, int radius) {
    if (x < 0 || y < 0 || x >= w || y >= h) return false;
    int dx = x < radius ? radius - 1 - x : x >= w - radius ? x - (w - radius) : 0;
    int dy = y < radius ? radius - 1 - y : y >= h - radius ? y - (h - radius) : 0;
    return dx * dx + dy * dy < radius * radius;
}
static void status(const char *message) {
    snprintf(notice, sizeof notice, "%s", message);
    notice_end = clock_ns() + 3000000000ull;
    dirty = 1;
}
static void rect(int x, int y, int w, int h, uint32_t fill) {
    int left = clamp(x, 0, WIDTH), right = clamp(x + w, 0, WIDTH);
    int top = clamp(y, 0, HEIGHT), bottom = clamp(y + h, 0, HEIGHT);
    for (int row = top; row < bottom; ++row)
        for (int col = left; col < right; ++col) canvas[row * WIDTH + col] = fill;
}
static void text_at(int x, int y, const char *value, uint32_t ink, int limit) {
    for (; *value; ++value) {
        unsigned ch = (unsigned char)*value;
        unsigned a = ch < 32 || ch > 126 ? '?' - 32 : ch - 32;
        if (x + advance[a] > limit) break;
        for (int row = 0; row < 17; ++row)
            for (int col = 0; col < 12; ++col)
                if ((glyphs[a][row] >> col & 1u) && x + col >= 0 && x + col < WIDTH &&
                    y + row >= 0 && y + row < HEIGHT)
                    canvas[(y + row) * WIDTH + x + col] = ink;
        x += advance[a];
    }
}
static void put_button(int which, const char *label) {
    const Button *b = &buttons[which];
    bool chosen = screen == EDIT && edit_button == which;
    uint32_t tint = chosen || (touch_button == which && b->keys) ? RGB(0x296b5b) : DARK;
    int w = b->size, h = button_height(which, b), radius = button_radius(which, b);
    unsigned alpha = b->opacity * 256 / 100;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (!inside_shape(x, y, w, h, radius)) continue;
            uint32_t *pixel = &canvas[(b->y + y) * WIDTH + b->x + x];
            if (chosen && !inside_shape(x - 2, y - 2, w - 4, h - 4, radius - 2)) {
                *pixel = ACCENT;
                continue;
            }
            uint32_t old = *pixel;
            unsigned rb = ((old & 0xff00ff) * (256 - alpha) + (tint & 0xff00ff) * alpha) >> 8;
            unsigned g = ((old & 0x00ff00) * (256 - alpha) + (tint & 0x00ff00) * alpha) >> 8;
            *pixel = RGB((rb & 0xff00ff) | (g & 0xff00));
        }
    }
    int label_width = 0;
    for (const char *p = label; *p; ++p) label_width += advance[*p - 32];
    text_at(b->x + (b->size - label_width) / 2, b->y + (h - 17) / 2,
            label, WHITE, b->x + b->size);
}
static void draw_game(void) {
    int w = WIDTH, h = HEIGHT, base_x = 0, base_y = 0;
    bool panel_is_narrower = WIDTH * cart_height <= HEIGHT * cart_width;
    if (scaling != STRETCH) {
        if (panel_is_narrower == (scaling == FIT)) h = WIDTH * cart_height / cart_width;
        else w = HEIGHT * cart_width / cart_height;
        base_x = (WIDTH - w) / 2;
        base_y = (HEIGHT - h) / 2;
    }
    int left = base_x < 0 ? 0 : base_x;
    int right = clamp(base_x + w, 0, WIDTH);
    int lookup[WIDTH];
    for (int x = left; x < right; ++x) lookup[x] = (x - base_x) * cart_width / w;
    memset(canvas, 0, sizeof canvas);
    for (int y = clamp(base_y, 0, HEIGHT); y < clamp(base_y + h, 0, HEIGHT); ++y) {
        const uint32_t *source = cart + ((y - base_y) * cart_height / h) * cart_width;
        uint32_t *dest = canvas + y * WIDTH;
        for (int x = left; x < right; ++x) dest[x] = source[lookup[x]];
    }
    for (int i = 0; i < CONTROLS; ++i) {
        if (!buttons[i].visible) continue;
        char label[12];
        const char *name = button_label(i, &buttons[i]);
        if (i == QUICK_SAVE || i == QUICK_LOAD) {
            snprintf(label, sizeof label, "%c%02d", i == QUICK_SAVE ? 'S' : 'L', slot + 1);
            name = label;
        }
        put_button(i, name);
    }
    if (fast) {
        rect(127, 7, 121, 23, DARK);
        char badge[32];
        if (measured_speed > 0)
            snprintf(badge, sizeof badge, "%.2fx / %dx", measured_speed, fast_rate);
        else snprintf(badge, sizeof badge, ">> %dx target", fast_rate);
        text_at(138, 10, badge, ACCENT, 244);
    }
}
static void header(const char *title, const char *sub) {
    rect(0, 0, WIDTH, HEIGHT, DARK);
    text_at(17, 10, title, ACCENT, WIDTH - 16);
    text_at(17, 31, sub, GRAY, WIDTH - 16);
}
static void row(int index, const char *name, bool selected) {
    int y = 47 + index * 43;
    rect(13, y, WIDTH - 26, 40, selected ? RGB(0x245448) : RGB(0x16282b));
    text_at(24, y + 11, name, WHITE, WIDTH - 25);
}
static void box(int x, int y, int w, const char *name, bool enabled) {
    rect(x, y, w, 38, enabled ? RGB(0x245448) : RGB(0x16282b));
    text_at(x + 8, y + 10, name, enabled ? WHITE : GRAY, x + w - 4);
}
static int file_path(char *path, size_t capacity, const char *base, const char *ext) {
    int n = snprintf(path, capacity, "%s%s", base, ext);
    return n >= 0 && (size_t)n < capacity;
}
static int file_write(const char *path, const void *data, size_t length) {
    char temp[PATH_CAP];
    if (!file_path(temp, sizeof temp, path, ".tmp")) return false;
    FILE *f = fopen(temp, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, length, f) == length && !fflush(f) && !fsync(fileno(f));
    if (fclose(f)) ok = false;
    if (ok && !rename(temp, path)) return true;
    unlink(temp);
    return false;
}
static void request_audio(enum AudioAction action, int value) {
    if (!audio_bridge || pending_action) return;
    bool opening = action == AUDIO_NOW_PLAYING;
    bool releasing = (action == AUDIO_MUSIC || opening) && !music_mode;
    if (opening) {
        platform_yield_to_player();
        physical_keys = pressed_this_frame = touch_keys = 0;
        held_buttons = gameplay_buttons = 0;
        native_yielded = true;
    }
    if (releasing) platform_audio_release();
    struct AudioCommand command = {AUDIO_MAGIC, ++audio_seq, action, value};
    if (!file_write(AUDIO_REQUEST, &command, sizeof command)) {
        if (releasing) platform_audio_select(audio_route, audio_reply.magic ? audio_reply.gain : 100);
        if (opening) { native_yielded = false; platform_resume_from_player(); }
        status("Q2 audio controls are unavailable");
        return;
    }
    pending_action = action;
    audio_request_time = clock_ns();
    dirty = 1;
}
static void audio_poll(uint64_t now) {
    if (!audio_bridge || now - audio_polled < 75000000ull) return;
    audio_polled = now;
    struct AudioReply next = {0};
    FILE *file = fopen(AUDIO_REPLY, "rb");
    if (file) {
        size_t bytes = fread(&next, 1, sizeof next, file);
        if (bytes != sizeof next && (native_supported || bytes != sizeof next - sizeof next.native_ui))
            next.magic = 0;
        fclose(file);
    }
    if (next.magic == AUDIO_MAGIC) {
        bool switched = music_mode != (next.music != 0);
        if (switched) {
            music_mode = next.music != 0;
            char path[PATH_CAP];
            const char *mode = music_mode ? "music\n" : "game\n";
            if (file_path(path, sizeof path, data_dir, "/audio-source.cfg"))
                (void)file_write(path, mode, strlen(mode));
            if (music_mode) platform_audio_release();
        }
        if (!music_mode && (switched || audio_route != next.route)) {
            audio_route = next.route;
            platform_audio_select(audio_route, next.gain);
        }
        if (!music_mode) platform_audio_gain(next.gain);
        if (pending_action && next.seq == audio_seq) {
            pending_action = 0;
            if (screen == AUDIO) dirty = 1;
        }
        if (native_yielded && !next.native_ui && !pending_action) {
            native_yielded = false;
            platform_resume_from_player();
            physical_keys = pressed_this_frame = touch_keys = 0;
            held_buttons = gameplay_buttons = 0;
            menu_focus = 0; /* Resume is the first row of the emulator menu. */
            dirty = 1;
        }
        if (memcmp(&next, &audio_reply, sizeof next)) {
            audio_reply = next;
            if (screen == AUDIO) dirty = 1;
        }
    }
    if (pending_action && now - audio_request_time > 5000000000ull) {
        if ((pending_action == AUDIO_MUSIC || pending_action == AUDIO_NOW_PLAYING) && !music_mode)
            platform_audio_select(audio_route, audio_reply.magic ? audio_reply.gain : 100);
        if (native_yielded) { native_yielded = false; platform_resume_from_player(); }
        pending_action = 0;
        status("Q2 audio request timed out");
    }
}
static void store_config(void) {
    char buffer[768];
    int used = snprintf(buffer, sizeof buffer, "%d 6 %d %d\n", scaling, fast_rate, local_volume);
    for (int i = 0; i < CONTROLS; ++i) {
        const Button *b = &buttons[i];
        used += snprintf(buffer + used, sizeof buffer - used, "%d %d %d %d %d %u\n",
                         b->x, b->y, b->size, b->opacity, b->visible, b->keys);
    }
    for (int i = 0; i < PHYSICAL_COUNT; ++i)
        used += snprintf(buffer + used, sizeof buffer - used, "%u\n", physical_map[i]);
    if (!file_write(config_file, buffer, used)) status("Could not save overlay settings");
}
static void load_config(void) {
    memcpy(buttons, defaults, sizeof buttons);
    memcpy(physical_map, physical_defaults, sizeof physical_map);
    FILE *f = fopen(config_file, "r");
    int saved_scale = FIT, version = 1, rate = 2, volume = 100;
    char line[80];
    if (!f) return;
    if (fgets(line, sizeof line, f)) sscanf(line, "%d %d %d %d", &saved_scale, &version, &rate, &volume);
    local_volume = clamp(volume, 0, 100);
    platform_audio_attenuation(local_volume);
    if (saved_scale >= FIT && saved_scale <= STRETCH) scaling = saved_scale;
    if (rate == 2 || rate == 3 || rate == 5) fast_rate = rate == 5 ? 3 : rate;
    int rows = version < 3 ? (int)(sizeof previous_overlays / sizeof previous_overlays[0]) : version < 6 ? TOUCH_A : CONTROLS;
    for (int row = 0; row < rows; ++row) {
        int x, y, size, alpha, show;
        unsigned keys = 0;
        if (!fgets(line, sizeof line, f) ||
            sscanf(line, "%d %d %d %d %d %u", &x, &y, &size, &alpha, &show, &keys) < (version < 6 ? 5 : 6))
            break;
        int which = version < 3 ? previous_overlays[row] : row;
        if (which < 0) continue;
        Button *b = &buttons[which];
        if (version < 2) {
            const int *old = legacy_geometry[row];
            if (x == old[0] && y == old[1]) { x = b->x; y = b->y; }
            if (size == old[2]) size = b->size;
        } else if (version == 2 && y == 260 &&
                   ((which == QUICK_SAVE && x == 122) || (which == QUICK_LOAD && x == 174))) {
            y = b->y;
        }
        if (version < 4) {
            if (which == MENU_BUTTON && size == 44) size = b->size;
            if (which == SPEED && y == 260 && (x == 66 || x == 70)) x = b->x;
        }
        b->size = clamp(size, 36, 88);
        move_button(which, x, y);
        b->opacity = clamp(alpha, 16, 100);
        b->visible = which == MENU_BUTTON || show != 0;
        if (version >= 6 && remappable_overlay(which) && valid_action(keys)) b->keys = keys;
    }
    if (version >= 6) {
        for (int i = 0; i < PHYSICAL_COUNT; ++i) {
            unsigned keys;
            if (!fgets(line, sizeof line, f)) break;
            if (sscanf(line, "%u", &keys) == 1 && valid_action(keys)) physical_map[i] = keys;
        }
    }
    fclose(f);
}
static bool state_path(char *path, int number) {
    char extension[24];
    snprintf(extension, sizeof extension, ".state%02d", number + 1);
    return file_path(path, PATH_CAP, game_base, extension);
}
static bool state_info(int number, time_t *when) {
    struct stat st;
    char path[PATH_CAP];
    if (!state_path(path, number) || stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return false;
    if (when) *when = st.st_mtime;
    return true;
}
static void battery_io(unsigned kind, const char *suffix, bool save) {
    char path[PATH_CAP];
    size_t size = core->get_memory_size(kind);
    void *data = core->get_memory_data(kind);
    if (!size || !data || !file_path(path, sizeof path, game_base, suffix)) return;
    if (save) {
        if (!file_write(path, data, size)) status("Could not write in-game save");
    } else {
        FILE *file = fopen(path, "rb");
        if (file) { (void)fread(data, 1, size, file); fclose(file); }
    }
}
static void cartridge_saves(bool save) {
    battery_io(RETRO_MEMORY_SAVE_RAM, ".srm", save);
    if (core == &gambatte) battery_io(RETRO_MEMORY_RTC, ".rtc", save);
}
static bool state_io(enum Action action, int number) {
    size_t length = core->serialize_size();
    char path[PATH_CAP];
    if (!length || !state_path(path, number)) return false;
    void *data = malloc(length);
    if (!data) return false;
    bool ok = false;
    if (action == SAVE) ok = core->serialize(data, length) && file_write(path, data, length);
    else {
        FILE *f = fopen(path, "rb");
        if (f) {
            ok = fread(data, 1, length, f) == length && fgetc(f) == EOF && !ferror(f);
            fclose(f);
            if (ok) ok = core->unserialize(data, length);
        }
    }
    free(data);
    return ok;
}
static void set_screen(enum Screen next) {
    if (!smoke && running && (screen == GAME || next == GAME)) platform_audio_reset();
    screen = next;
    physical_keys = gameplay_buttons = 0;
    menu_focus = wheel_delta = 0;
    pressed_this_frame = 0;
    speed_start = 0;
    next_video = 0;
    measured_speed = 0;
    touch_keys = 0;
    touch_button = -1;
    dirty = 1;
}
static bool do_state(enum Action a) {
    bool ok = state_io(a, slot);
    char message[60];
    snprintf(message, sizeof message, "%s slot %02d",
             ok ? (a == SAVE ? "Saved to" : "Loaded") : (a == SAVE ? "Could not save" : "Could not load"),
             slot + 1);
    status(message);
    return ok;
}
static void ask_state(enum Action a) {
    bool exists = state_info(slot, NULL);
    if (a == LOAD && !exists) { status("This slot is empty"); return; }
    if (a == LOAD || exists) {
        confirm_action = a;
        confirm_from = screen;
        set_screen(CONFIRM);
    } else if (do_state(a) && screen != GAME) set_screen(GAME);
}
static void set_fast(void) {
    fast = !fast;
    speed_start = 0;
    next_video = 0;
    core_options_changed = true;
    measured_speed = 0;
    if (fast) fprintf(stderr, "Fast-forward on (%dx target)\n", fast_rate);
    else fprintf(stderr, "Fast-forward off\n");
    platform_audio_reset();
    dirty = 1;
}
static int rom_compare(const void *a, const void *b) { return strcasecmp(a, b); }
static void scan_roms(void) {
    rom_count = 0;
    DIR *dir = opendir(rom_dir);
    if (!dir) return;
    struct dirent *entry;
    while ((entry = readdir(dir)) && rom_count < ROM_LIMIT) {
        if (entry->d_name[0] == '.' || rom_type(entry->d_name) == ROM_UNKNOWN) continue;
        snprintf(roms[rom_count++], sizeof roms[0], "%s", entry->d_name);
    }
    closedir(dir);
    qsort(roms, rom_count, sizeof roms[0], rom_compare);
    selection = clamp(selection, 0, rom_count ? rom_count - 1 : 0);
}
static bool load_rom(const char *path) {
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    int n = snprintf(game_base, sizeof game_base, "%s/%s", save_dir, name);
    if (n < 0 || (size_t)n >= sizeof game_base) return false;
    struct stat file;
    if (stat(path, &file) || !S_ISREG(file.st_mode) || file.st_size < 192 ||
        (uint64_t)file.st_size > 32 * 1024 * 1024ull) return false;
    enum RomType type = rom_type(path);
    if (type == ROM_UNKNOWN) return false;
    if (type == ROM_GB && !enable_gambatte()) {
        status("Game Boy core missing from .boy folder");
        return false;
    }
    initialize_core(type == ROM_GBA ? &gpsp : &gambatte);
    struct retro_game_info info = {.path = path};
    void *rom_data = NULL;
    if (type == ROM_GB) {
        rom_data = malloc(file.st_size);
        FILE *rom = rom_data ? fopen(path, "rb") : NULL;
        bool read = rom && fread(rom_data, 1, file.st_size, rom) == (size_t)file.st_size;
        if (rom) fclose(rom);
        if (!read) { free(rom_data); release_core(); return false; }
        info.data = rom_data;
        info.size = file.st_size;
    }
    fprintf(stderr, "Opening %s (%ld bytes), core=%s\n", name, (long)file.st_size,
            type == ROM_GB ? "Gambatte" : use_dynarec ? "gpSP dynarec" : "gpSP interpreter");
    launch_stage = 1;
    bool loaded = core->load_game(&info);
    free(rom_data); /* Gambatte copies the cartridge on load. */
    if (!loaded) {
        launch_stage = 0;
        release_core();
        return false;
    }
    launch_stage = 2;
    struct retro_system_av_info av = {0};
    core->get_system_av_info(&av);
    fps = av.timing.fps > 50 && av.timing.fps < 65 ? av.timing.fps : 59.7275;
    cart_width = av.geometry.base_width;
    cart_height = av.geometry.base_height;
    if (!cart_width || !cart_height || cart_width > CART_WIDTH || cart_height > CART_HEIGHT) {
        core->unload_game();
        release_core();
        return false;
    }
    if (!smoke && av.timing.sample_rate >= 8000 && av.timing.sample_rate <= 192000)
        platform_audio_set_rate((unsigned)(av.timing.sample_rate + 0.5));
    cartridge_saves(false);
    running = 1;
    slot = 0;
    fast = 0;
    have_frame = false;
    memset(cart, 0, sizeof cart);
    next_save = clock_ns() + 20000000000ull;
    set_screen(GAME);
    fprintf(stderr, "ROM loaded: %s (%.3f fps)\n", name, fps);
    return true;
}
static void launch_selected(void) {
    char path[PATH_CAP];
    notice_end = 0;
    if (rom_count && snprintf(path, sizeof path, "%s/%s", rom_dir, roms[selection]) < (int)sizeof path &&
        !load_rom(path) && notice_end <= clock_ns()) status("Could not open this ROM");
}
static void close_rom(void) {
    if (running) {
        launch_stage = 5;
        cartridge_saves(true);
        core->unload_game();
    }
    launch_stage = 0;
    release_core();
    running = 0;
    fast = 0;
    if (!smoke) platform_audio_reset();
}
typedef struct { int x, y, w, h; bool enabled; char label[96]; } MenuItem;

static int menu_items(MenuItem *items) {
    int count = 0;
    char label[96];
#define ITEM(x_, y_, w_, h_, label_, enabled_) do { \
    MenuItem *m = &items[count++]; \
    *m = (MenuItem){x_, y_, w_, h_, enabled_, {0}}; \
    snprintf(m->label, sizeof m->label, "%s", (label_)); \
} while (0)
    switch (screen) {
    case MENU:
        ITEM(15, 50, 170, 41, "Resume", true);
        ITEM(190, 50, 170, 41, "Q2 Now Playing", native_supported && !pending_action);
        ITEM(15, 94, 345, 41, "Save states", true);
        ITEM(15, 138, 345, 41, fast ? "Turn fast-forward off" : "Turn fast-forward on", true);
        snprintf(label, sizeof label, "Fast-forward speed: %dx", fast_rate);
        ITEM(15, 182, 345, 41, label, true);
        ITEM(15, 226, 170, 41, "Display and controls", true);
        ITEM(190, 226, 170, 41, "Audio", true);
        ITEM(15, 270, 170, 41, "ROM library", true);
        ITEM(190, 270, 170, 41, "Exit to Q2-Pod", true);
        break;
    case STATES:
        ITEM(15, 101, 168, 53, "Previous slot", true);
        ITEM(192, 101, 168, 53, "Next slot", true);
        ITEM(15, 163, 168, 57, "Save state", true);
        ITEM(192, 163, 168, 57, "Load state", state_info(slot, NULL));
        ITEM(15, 235, 345, 51, "Back", true);
        break;
    case SETTINGS: {
        static const char *const modes[] = {"Original aspect", "Crop to fill", "Stretch to fill"};
        snprintf(label, sizeof label, "Picture: %s", modes[scaling]);
        ITEM(15, 53, 345, 43, label, true);
        ITEM(15, 103, 345, 43, "Physical buttons", true);
        ITEM(15, 153, 345, 43, "Touch buttons", true);
        ITEM(15, 203, 345, 44, "Move, resize, or fade buttons", true);
        ITEM(15, 258, 132, 48, "Back", true);
        ITEM(152, 258, 208, 48, "Reset controls", true);
        break;
    }
    case PHYSICAL:
        for (int i = 0; i < PHYSICAL_COUNT; ++i) {
            snprintf(label, sizeof label, "%s: %s", physical_names[i], action_name(physical_map[i]));
            ITEM(15, 49 + i * 43, 345, 39, label, true);
        }
        ITEM(15, 267, 345, 40, "Back", true);
        break;
    case TOUCH:
    case ASSIGN: {
        int page = screen == TOUCH ? touch_page : assign_page;
        int total = screen == TOUCH ? (int)(sizeof touch_choices / sizeof touch_choices[0])
                                    : (int)(sizeof actions / sizeof actions[0]);
        for (int i = 0; i < 6; ++i) {
            int k = page * 6 + i;
            if (k >= total) break;
            if (screen == TOUCH) {
                int id = touch_choices[k];
                snprintf(label, sizeof label, "%s: %s", button_label(id, &buttons[id]),
                         buttons[id].visible ? "On" : "Off");
            } else snprintf(label, sizeof label, "%s", actions[k].name);
            ITEM(15 + (i % 2) * 175, 58 + (i / 2) * 60, 170, 51, label, true);
        }
        ITEM(15, 252, 110, 49, "Previous page", page > 0);
        ITEM(132, 252, 110, 49, "Next page", (page + 1) * 6 < total);
        ITEM(249, 252, 111, 49, "Back", true);
        break;
    }
    case EDIT:
        ITEM(14, 100, 346, 42, "Choose next overlay", true);
        ITEM(10, 148, 68, 44, "Size -", true);
        ITEM(82, 148, 68, 44, "Size +", true);
        ITEM(154, 148, 68, 44, "Fade -", true);
        ITEM(226, 148, 68, 44, "Fade +", true);
        ITEM(298, 148, 67, 44, "Done", true);
        ITEM(14, 198, 182, 40, "Assign action", remappable_overlay(edit_button));
        break;
    case AUDIO:
        ITEM(15, 54, 168, 51, music_mode ? "Game audio" : "Game audio (on)", audio_bridge && music_mode && !pending_action);
        ITEM(192, 54, 168, 51, music_mode ? "Q2 music (on)" : "Q2 music", audio_bridge && !music_mode && !pending_action);
        ITEM(15, 123, 168, 55, "Game volume -", local_volume > 0);
        ITEM(192, 123, 168, 55, "Game volume +", local_volume < 100);
        ITEM(15, 235, 345, 52, "Back to emulator menu", true);
        break;
    case CONFIRM:
        ITEM(15, 98, 169, 69, "Cancel", true);
        ITEM(192, 98, 168, 69, confirm_action == SAVE ? "Overwrite" : "Load state", true);
        break;
    default: break;
    }
#undef ITEM
    return count;
}
static void draw_ui(void) {
    char label[256], sub[128];
    switch (screen) {
    case GAME: draw_game(); break;
    case BROWSER: {
        header("q2-boy", rom_count ? "Choose a cartridge" : "Copy .gb, .gbc or .gba files to q2-boy/ROMs on the SD card");
        int start = (selection / ROWS) * ROWS;
        for (int i = 0; i < ROWS && start + i < rom_count; ++i)
            row(i, roms[start + i], start + i == selection);
        box(14, 270, 75, "Previous", start > 0);
        box(97, 270, 75, "Next", start + ROWS < rom_count);
        box(178, 270, 75, "Refresh", true);
        box(273, 270, 88, "Exit", true);
        break;
    }
    case MENU:
        header("Emulator Menu", "Scroll to highlight. Press Centre to select.");
        break;
    case STATES: {
        time_t when;
        snprintf(sub, sizeof sub, "Slot %02d of %d", slot + 1, SLOTS);
        header("Save states", sub);
        if (state_info(slot, &when)) {
            struct tm *tm = localtime(&when);
            if (!tm || !strftime(label, sizeof label, "%Y-%m-%d  %H:%M:%S", tm))
                snprintf(label, sizeof label, "Saved");
        } else snprintf(label, sizeof label, "Empty slot");
        text_at(17, 64, label, WHITE, WIDTH - 15);
        break;
    }
    case SETTINGS:
        header("Display and controls", "Scroll and press Centre, or tap a setting.");
        break;
    case PHYSICAL:
        header("Physical buttons", "Choose a button to change its gameplay action.");
        break;
    case TOUCH:
        header("Touch buttons", "Choose which overlays appear during gameplay.");
        break;
    case ASSIGN:
        header("Assign action", assign_return == PHYSICAL ? physical_names[assign_target] : "Touch button");
        break;
    case EDIT:
        draw_game();
        rect(0, 55, WIDTH, 38, DARK);
        text_at(12, 57, "Drag an overlay or choose an action.", WHITE, WIDTH - 10);
        snprintf(label, sizeof label, "%s  %d px  %d%% opacity",
                 button_label(edit_button, &buttons[edit_button]), buttons[edit_button].size, buttons[edit_button].opacity);
        text_at(12, 73, label, ACCENT, WIDTH - 10);
        break;
    case AUDIO:
        header("Audio", pending_action == AUDIO_GAME || pending_action == AUDIO_MUSIC
                       ? "Switching output..." : music_mode ? "Q2 music is selected" : "Game audio is selected");
        snprintf(label, sizeof label, "Game volume: %d%%", local_volume);
        text_at(17, 194, label, WHITE, WIDTH - 16);
        break;
    case CONFIRM:
        snprintf(label, sizeof label, confirm_action == SAVE ? "Overwrite slot %02d?" : "Load slot %02d?",
                 slot + 1);
        header(label, confirm_action == SAVE ? "This replaces the saved state in that slot."
                                             : "Unsaved progress in this session will be lost.");
        break;
    }
    MenuItem items[9];
    int count = menu_items(items);
    for (int i = 0; i < count; ++i) {
        const MenuItem *m = &items[i];
        bool focused = m->enabled && i == menu_focus;
        uint32_t fill = focused ? RGB(0x245448) : RGB(0x16282b);
        rect(m->x, m->y, m->w, m->h, fill);
        if (focused) rect(m->x, m->y, 4, m->h, ACCENT);
        text_at(m->x + 11, m->y + (m->h - 17) / 2, m->label, m->enabled ? WHITE : GRAY,
                m->x + m->w - 6);
    }
    if (notice_end && clock_ns() < notice_end) {
        rect(37, HEIGHT - 29, WIDTH - 74, 23, DARK);
        text_at(45, HEIGHT - 26, notice, ACCENT, WIDTH - 42);
    }
    if (!smoke) platform_present(canvas, paced_video());
    dirty = 0;
}

static int hit(int x, int y) {
    for (int i = CONTROLS - 1; i >= 0; --i) {
        const Button *b = &buttons[i];
        if (b->visible && inside_shape(x - b->x, y - b->y, b->size,
                                       button_height(i, b), button_radius(i, b))) return i;
    }
    return -1;
}
static bool within(int x, int y, int left, int top, int width) {
    return x >= left && x < left + width && y >= top && y < top + 38;
}
static void go_back(void) {
    switch (screen) {
    case BROWSER: quitting = 1; break;
    case MENU: set_screen(GAME); break;
    case GAME: set_screen(MENU); break;
    case STATES: case SETTINGS: case AUDIO: set_screen(MENU); break;
    case EDIT: dragging = -1; store_config(); set_screen(SETTINGS); break;
    case CONFIRM: set_screen(confirm_from); break;
    case PHYSICAL: case TOUCH: set_screen(SETTINGS); break;
    case ASSIGN: set_screen(assign_return); break;
    }
}
static void slot_step(int step) { slot = (slot + step + SLOTS) % SLOTS; dirty = 1; }
static void pick_rom(int step) {
    if (rom_count) selection = clamp(selection + step, 0, rom_count - 1);
    dirty = 1;
}
static void activate_menu(int index) {
    MenuItem items[9];
    int count = menu_items(items);
    if (index < 0 || index >= count || !items[index].enabled) return;
    menu_focus = index;
    switch (screen) {
    case MENU:
        switch (index) {
        case 0: set_screen(GAME); break;
        case 1: request_audio(AUDIO_NOW_PLAYING, 0); break;
        case 2: set_screen(STATES); break;
        case 3: set_fast(); set_screen(GAME); break;
        case 4:
            fast_rate = fast_rate == speeds[0] ? speeds[1] : speeds[0];
            core_options_changed = true;
            store_config();
            break;
        case 5: set_screen(SETTINGS); break;
        case 6:
            set_screen(AUDIO);
            menu_focus = audio_bridge ? (music_mode ? 0 : 1) : 2;
            if (!audio_bridge && !local_volume) menu_focus = 3;
            break;
        case 7: close_rom(); scan_roms(); set_screen(BROWSER); break;
        case 8: quitting = 1; break;
        }
        break;
    case STATES:
        if (index < 2) slot_step(index == 0 ? -1 : 1);
        else if (index == 2) ask_state(SAVE);
        else if (index == 3) ask_state(LOAD);
        else go_back();
        break;
    case SETTINGS:
        if (index == 0) { scaling = (scaling + 1) % 3; store_config(); }
        else if (index == 1) set_screen(PHYSICAL);
        else if (index == 2) { touch_page = 0; set_screen(TOUCH); }
        else if (index == 3) { edit_button = MENU_BUTTON; set_screen(EDIT); }
        else if (index == 4) go_back();
        else {
            memcpy(buttons, defaults, sizeof buttons);
            memcpy(physical_map, physical_defaults, sizeof physical_map);
            store_config();
            status("Default controls restored");
        }
        break;
    case PHYSICAL:
        if (index == PHYSICAL_COUNT) go_back();
        else { assign_target = index; assign_page = 0; assign_return = PHYSICAL; set_screen(ASSIGN); }
        break;
    case TOUCH: case ASSIGN: {
        bool touch = screen == TOUCH;
        int *page = touch ? &touch_page : &assign_page;
        if (index == count - 1) go_back();
        else if (index == count - 2 || index == count - 3) {
            *page += index == count - 2 ? 1 : -1;
            menu_focus = 0;
        } else {
            int k = *page * 6 + index;
            if (touch) buttons[touch_choices[k]].visible ^= 1;
            else if (assign_return == PHYSICAL) physical_map[assign_target] = actions[k].keys;
            else buttons[assign_target].keys = actions[k].keys;
            store_config();
            if (!touch) go_back();
        }
        break;
    }
    case EDIT: {
        Button *b = &buttons[edit_button];
        if (index == 0) {
            do { edit_button = (edit_button + 1) % CONTROLS; } while (!buttons[edit_button].visible);
        } else if (index <= 2) {
            b->size = clamp(b->size + (index == 1 ? -4 : 4), 36, 88);
            move_button(edit_button, b->x, b->y);
        } else if (index <= 4) {
            b->opacity = clamp(b->opacity + (index == 3 ? -8 : 8), 16, 100);
        } else if (index == 5) go_back();
        else { assign_target = edit_button; assign_page = 0; assign_return = EDIT; set_screen(ASSIGN); }
        break;
    }
    case AUDIO:
        if (index == 0 || index == 1) request_audio(index ? AUDIO_MUSIC : AUDIO_GAME, 0);
        else if (index == 2 || index == 3) {
            local_volume = clamp(local_volume + (index == 2 ? -5 : 5), 0, 100);
            platform_audio_attenuation(local_volume);
            store_config();
        } else go_back();
        break;
    case CONFIRM:
        if (index == 0 || !do_state(confirm_action)) go_back();
        else set_screen(GAME);
        break;
    default: break;
    }
    dirty = 1;
}
static void tap_menu(int x, int y) {
    MenuItem items[9];
    int count = menu_items(items);
    for (int i = 0; i < count; ++i) {
        const MenuItem *m = &items[i];
        if (x >= m->x && y >= m->y && x < m->x + m->w && y < m->y + m->h) {
            activate_menu(i);
            return;
        }
    }
}
static void step_menu(int step) {
    MenuItem items[9];
    int count = menu_items(items);
    if (!count) return;
    do { menu_focus = (menu_focus + step + count) % count; } while (!items[menu_focus].enabled);
    dirty = 1;
}
static void read_touch(char action, int x, int y) {
    if (screen == EDIT && (action == 'm' || action == 'u')) {
        if (abs(x - edit_start_x) + abs(y - edit_start_y) >= 5) edit_moved = 1;
        if (action == 'm' && dragging >= 0 && edit_moved) {
            move_button(dragging, x - drag_x, y - drag_y);
        }
        if (action == 'u') {
            if (!edit_moved) tap_menu(x, y);
            dragging = -1;
        }
        dirty = 1;
        return;
    }
    if (action == 'u') { touch_keys = 0; touch_button = -1; dirty = 1; }
    if (action != 'd') return;
    int item = y >= 47 && y < 47 + ROWS * 43 && (y - 47) % 43 < 40 ? (y - 47) / 43 : -1;
    if (x < 13 || x >= WIDTH - 13) item = -1;
    switch (screen) {
    case GAME: {
        int which = hit(x, y);
        if (which < 0) break;
        touch_button = which;
        if (buttons[which].keys) {
            touch_keys = buttons[which].keys;
            pressed_this_frame |= touch_keys;
        }
        if (which == MENU_BUTTON) set_screen(MENU);
        if (which == SPEED) set_fast();
        if (which == QUICK_SAVE) ask_state(SAVE);
        if (which == QUICK_LOAD) ask_state(LOAD);
        break;
    }
    case BROWSER: {
        int idx = (selection / ROWS) * ROWS + item;
        if (item >= 0 && idx < rom_count) { selection = idx; launch_selected(); }
        else if (within(x, y, 14, 270, 75)) pick_rom(-ROWS);
        else if (within(x, y, 97, 270, 75)) pick_rom(ROWS);
        else if (within(x, y, 178, 270, 75)) scan_roms();
        else if (within(x, y, 273, 270, 88)) go_back();
        break;
    }
    case MENU: case STATES: case SETTINGS: case CONFIRM: case AUDIO: case PHYSICAL: case TOUCH: case ASSIGN:
        tap_menu(x, y);
        break;
    case EDIT: {
        edit_start_x = x;
        edit_start_y = y;
        edit_moved = 0;
        int which = hit(x, y);
        if (which >= 0) {
            edit_button = dragging = which;
            drag_x = x - buttons[which].x;
            drag_y = y - buttons[which].y;
        }
        break;
    }
    }
    dirty = 1;
}
static void read_wheel(int delta) {
    if (!delta || screen == GAME) return;
    uint64_t now = clock_ns();
    if (!wheel_time || now - wheel_time > 300000000ull) wheel_delta = 0;
    wheel_time = now;
    wheel_delta += clamp(delta, -84, 84);
    while (abs(wheel_delta) >= 21) {
        int step = wheel_delta < 0 ? 1 : -1;
        wheel_delta += step * 21;
        if (screen == BROWSER) pick_rom(step);
        else step_menu(step);
    }
}
static int decode_key(Event e) {
    if (e.kind == 'g') {
        switch (e.x) { // Linux md-gpio-keys codes
        case 103: return P_RETURN;
        case 108: return P_PLAY;
        case 165: return P_PREVIOUS;
        case 163: return P_NEXT;
        case 116: return P_CENTRE;
        }
    }
    if (e.kind == 'k' && !raw_buttons) {
        switch (e.x) { // Q2-Pod card game codes
        case 170: return P_RETURN;
        case 171: return P_PLAY;
        case 222: return P_PREVIOUS;
        case 223: return P_NEXT;
        case 218: return P_CENTRE;
        }
    }
    return -1;
}
static void read_event(Event e) {
    if (e.kind == 'w') { read_wheel(e.x); return; }
    if (e.kind == 't') { read_touch(e.action, e.x, e.y); return; }
    if (e.kind == 'r') {
        physical_keys = pressed_this_frame = held_buttons = gameplay_buttons = 0;
        return;
    }
    int id = decode_key(e), down = e.action == 'd';
    if (id < 0 || (e.action != 'u' && !down)) return;
    unsigned bit = 1u << id;
    if (down) {
        if (held_buttons & bit) return;
        held_buttons |= bit;
        if (screen == GAME) {
            gameplay_buttons |= bit;
            pressed_this_frame |= physical_map[id];
        } else if (id == P_CENTRE) {
            if (screen == BROWSER) launch_selected();
            else activate_menu(menu_focus);
        } else if (screen == BROWSER) {
            if (id == P_RETURN) pick_rom(-1);
            if (id == P_PLAY) pick_rom(1);
            if (id == P_PREVIOUS) pick_rom(-ROWS);
            if (id == P_NEXT) pick_rom(ROWS);
        } else if (id == P_RETURN) go_back();
    } else {
        held_buttons &= ~bit;
        gameplay_buttons &= ~bit;
    }
    physical_keys = 0;
    for (int i = 0; i < PHYSICAL_COUNT; ++i)
        if (gameplay_buttons & (1u << i)) physical_keys |= physical_map[i];
}
static void log_core(enum retro_log_level level, const char *message, ...) {
    if (level < RETRO_LOG_INFO) return;
    va_list args;
    va_start(args, message);
    vfprintf(stderr, message, args);
    va_end(args);
}
static bool environment(unsigned command, void *data) {
    switch (command) {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        return *(enum retro_pixel_format *)data == RETRO_PIXEL_FORMAT_RGB565;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        *(const char **)data = data_dir;
        return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        struct retro_variable *v = data;
        if (!strcmp(v->key, "gpsp_drc")) v->value = use_dynarec ? "enabled" : "disabled";
        else if (!strcmp(v->key, "gpsp_rtc_time_source")) v->value = "system";
        else if (!strcmp(v->key, "gpsp_frameskip"))
            v->value = fast && fast_rate == 2 ? "fixed_interval" : "disabled";
        else if (!strcmp(v->key, "gpsp_frameskip_interval"))
            v->value = "1";
        else { v->value = NULL; return false; }
        return true;
    }
    case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:
        *(int *)data = 2 | (allow_video ? 1 : 0);
        return true;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE: *(bool *)data = true; return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        *(bool *)data = core_options_changed;
        core_options_changed = false;
        return true;
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS: return true;
    case RETRO_ENVIRONMENT_GET_FASTFORWARDING: *(bool *)data = fast != 0; return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        ((struct retro_log_callback *)data)->log = log_core;
        return true;
    default: return false;
    }
}
static void video_frame(const void *data, unsigned w, unsigned h, size_t pitch) {
    if (!data || w != cart_width || h != cart_height || pitch < w * sizeof(uint16_t) ||
        (core == &gambatte && !allow_video)) return;
    ++video_frames;
    for (unsigned y = 0; y < h; ++y) {
        const uint16_t *line = (const uint16_t *)((const char *)data + y * pitch);
        uint32_t *dest = cart + y * w;
        for (unsigned x = 0; x < w; ++x) dest[x] = color[line[x]];
    }
    if (!have_frame) fprintf(stderr, "First cartridge video frame received\n");
    have_frame = true;
    dirty = 1;
}
static size_t audio_batch(const int16_t *data, size_t frames) {
    if (smoke) smoke_audio_frames += frames;
    else {
        sig_atomic_t previous = launch_stage;
        launch_stage = 6;
        platform_audio_write(data, frames, music_mode || fast || screen != GAME);
        launch_stage = previous;
    }
    return frames;
}
static void audio_sample(int16_t l, int16_t r) {
    int16_t pair[] = {l, r};
    audio_batch(pair, 1);
}
static void input_poll(void) {}
static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id) {
    if (port || device != RETRO_DEVICE_JOYPAD || index) return 0;
    unsigned pressed = physical_keys | touch_keys | pressed_this_frame;
    if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return (int16_t)pressed;
    return id < 16 && (pressed & (1u << id)) != 0;
}
static void initialize_palette(void) {
    for (unsigned i = 0; i < 65536; ++i) {
        unsigned r = (i >> 11) & 31, g = (i >> 5) & 63, b = i & 31;
        color[i] = RGB((r * 255 / 31 << 16) | (g * 255 / 63 << 8) | (b * 255 / 31));
    }
}
static void initialize_core(const struct Core *selected) {
    core = selected;
    allow_video = true;
    core->set_environment(environment);
    core->set_video_refresh(video_frame);
    core->set_audio_sample(audio_sample);
    core->set_audio_sample_batch(audio_batch);
    core->set_input_poll(input_poll);
    core->set_input_state(input_state);
    core->init();
    core->set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
}
static bool prepare_dirs(void) {
    if (!file_path(rom_dir, sizeof rom_dir, data_dir, "/ROMs") ||
        !file_path(save_dir, sizeof save_dir, data_dir, "/Saves") ||
        !file_path(config_file, sizeof config_file, data_dir, "/controls.cfg")) return false;
    const char *dirs[] = {data_dir, rom_dir, save_dir};
    for (unsigned i = 0; i < sizeof dirs / sizeof dirs[0]; ++i) {
        struct stat st;
        if (mkdir(dirs[i], 0755) && (stat(dirs[i], &st) || !S_ISDIR(st.st_mode))) return false;
    }
    return true;
}
static int smoke_test(const char *path) {
    if (!load_rom(path)) return 1;
    smoke_audio_frames = 0;
    for (int i = 0; i < 15; ++i) core->run();
    if (!have_frame || !smoke_audio_frames ||
        (core == &gpsp && cart[80 * CART_WIDTH + 120] != RGB(0xff0000)) ||
        (core == &gambatte && (cart_width != 160 || cart_height != 144))) {
        close_rom();
        return 2;
    }
    if (!state_io(SAVE, 0)) { close_rom(); return 3; }
    for (int i = 0; i < 15; ++i) core->run();
    if (!state_io(SAVE, 1)) { close_rom(); return 4; }
    close_rom();
    if (!load_rom(path) || !state_io(LOAD, 0) || !state_io(LOAD, 1)) { close_rom(); return 5; }
    core->run();
    read_event((Event){'g', 'd', 103, 0});
    read_event((Event){'g', 'd', 163, 0});
    read_event((Event){'g', 'd', 116, 0});
    const Button *b = &buttons[B];
    read_touch('d', b->x + b->size / 2, b->y + b->size / 2);
    if (input_state(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK) !=
        (KEY(UP) | KEY(RIGHT) | KEY(A) | KEY(B))) { close_rom(); return 6; }
    read_event((Event){'g', 'u', 116, 0});
    read_touch('u', b->x + b->size / 2, b->y + b->size / 2);
    physical_keys = pressed_this_frame = held_buttons = gameplay_buttons = 0;
    int initial_game_volume = local_volume;
    read_wheel(200);
    if (local_volume != initial_game_volume) { close_rom(); return 18; }
    if (input_state(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK)) {
        close_rom();
        return 7;
    }
    for (int i = SELECT_BUTTON; i <= START_BUTTON; ++i) {
        const Button *overlay = &buttons[i];
        int x = overlay->x + overlay->size / 2;
        int y = overlay->y + button_height(i, overlay) / 2;
        read_touch('d', x, y);
        if (input_state(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK) != (int16_t)overlay->keys) {
            close_rom();
            return 8;
        }
        read_touch('u', x, y);
        pressed_this_frame = 0;
    }
    for (int i = FIT; i <= STRETCH; ++i) { scaling = i; draw_ui(); }
    set_screen(MENU);
    read_wheel(-21);
    read_event((Event){'g', 'd', 116, 0});
    read_event((Event){'g', 'u', 116, 0});
    if (screen != STATES) { close_rom(); return 9; }
    read_wheel(-42);
    read_event((Event){'g', 'd', 116, 0});
    read_event((Event){'g', 'u', 116, 0});
    if (screen != CONFIRM || confirm_action != SAVE) { close_rom(); return 10; }
    read_wheel(-21);
    read_event((Event){'g', 'd', 116, 0});
    if (screen != GAME || (physical_keys & KEY(A))) { close_rom(); return 13; }
    read_event((Event){'g', 'u', 116, 0});
    set_screen(STATES);
    read_touch('d', 240, 183);
    if (screen != CONFIRM || confirm_action != LOAD) { close_rom(); return 14; }
    read_touch('d', 240, 130);
    if (screen != GAME) { close_rom(); return 15; }
    set_screen(AUDIO);
    local_volume = 100;
    read_touch('d', 48, 150);
    if (local_volume != 95) { close_rom(); return 19; }
    load_config();
    if (local_volume != 95) { close_rom(); return 20; }
    set_screen(MENU);
    fast_rate = 2;
    read_touch('d', 150, 200);
    if (fast_rate != 3) { close_rom(); return 16; }
    fast_rate = 2;
    load_config();
    if (fast_rate != 3) { close_rom(); return 17; }
    set_screen(PHYSICAL);
    activate_menu(P_CENTRE);
    activate_menu(2); /* Start */
    activate_menu(P_NEXT);
    activate_menu(2); /* Duplicate bindings must release independently. */
    set_screen(TOUCH);
    touch_page = 0;
    activate_menu(0); /* Show A. */
    activate_menu(7); /* Next page. */
    activate_menu(0); /* Show Up. */
    edit_button = TOUCH_A;
    set_screen(EDIT);
    activate_menu(6); /* Assign action. */
    activate_menu(3); /* Select */
    load_config();
    if (physical_map[P_CENTRE] != KEY(START) || !buttons[TOUCH_A].visible ||
        buttons[TOUCH_A].keys != KEY(SELECT) || !buttons[TOUCH_UP].visible) {
        close_rom();
        return 21;
    }
    set_screen(GAME);
    read_event((Event){'g', 'd', 116, 0});
    read_event((Event){'g', 'd', 163, 0});
    read_event((Event){'g', 'u', 116, 0});
    pressed_this_frame = 0;
    if (physical_keys != KEY(START)) { close_rom(); return 22; }
    read_event((Event){'g', 'u', 163, 0});
    if (physical_keys) { close_rom(); return 23; }
    set_screen(MENU);
    read_event((Event){'g', 'd', 116, 0}); /* Centre still confirms in a menu. */
    if (screen != GAME || physical_keys || pressed_this_frame) { close_rom(); return 24; }
    read_event((Event){'g', 'u', 116, 0});
    set_screen(SETTINGS);
    activate_menu(5); /* Reset controls. */
    if (physical_map[P_CENTRE] != KEY(A) || buttons[TOUCH_A].visible ||
        !buttons[B].visible) { close_rom(); return 25; }
    set_screen(GAME);
    fast = 1;
    for (unsigned i = 0; i < sizeof speeds / sizeof speeds[0]; ++i) {
        fast_rate = speeds[i];
        core_options_changed = true;
        next_video = 0;
        schedule_video(1000000000ull);
        core->run();
        video_frames = 0;
        unsigned expected = 0;
        uint64_t period = 1000000000ull / (60 * fast_rate);
        for (unsigned frame = 1; frame <= 30; ++frame) {
            schedule_video(1000000000ull + frame * period);
            if (allow_video) ++expected;
            core->run();
        }
        if (fast_rate == 2 && core == &gpsp) expected = 15;
        if (video_frames != expected ||
            (fast_rate >= 3 && (expected < 2 || expected > 6))) { close_rom(); return 11; }
    }
    fast = 0;
    core_options_changed = true;
    schedule_video(2000000000ull);
    core->run();
    video_frames = 0;
    for (int frame = 0; frame < 6; ++frame) core->run();
    if (video_frames != 6) { close_rom(); return 12; }
    close_rom();
    puts("PASS: ROM, two save slots, new controls, wheel disabled during gameplay, wheel menus, instant state return, speed selection, persistent remapping, timed core video budget");
    return 0;
}
static void write_fatal(int sig) {
    const char *names[] = {"menu/initialization", "ROM load", "save RAM load",
                           "first emulated frame", "gameplay", "save/exit", "ALSA output"};
    char line[120] = "Fatal signal ";
    unsigned n = sizeof "Fatal signal " - 1;
    line[n++] = '0' + (sig / 10) % 10;
    line[n++] = '0' + sig % 10;
    line[n++] = ' ';
    line[n++] = 'i';
    line[n++] = 'n';
    line[n++] = ' ';
    unsigned stage = launch_stage;
    if (stage >= sizeof names / sizeof names[0]) stage = 0;
    for (const char *c = names[stage]; *c; ++c) line[n++] = *c;
    line[n++] = '\n';
    (void)write(STDERR_FILENO, line, n);
    _exit(128 + sig);
}
static void log_launch(void) {
    char log[PATH_CAP], previous[PATH_CAP], flag[PATH_CAP];
    if (file_path(log, sizeof log, data_dir, "/q2-boy.log")) {
        struct stat st;
        if (!stat(log, &st) && st.st_size > 128 * 1024 &&
            file_path(previous, sizeof previous, log, ".previous"))
            (void)rename(log, previous);
        FILE *stream = freopen(log, "a", stderr);
        if (stream) setvbuf(stream, NULL, _IOLBF, 0);
    }
    if (file_path(flag, sizeof flag, data_dir, "/enable-dynarec.txt")) {
        FILE *f = fopen(flag, "r");
        if (f) { use_dynarec = fgetc(f) == '1'; fclose(f); }
    }
    struct sigaction action = {.sa_handler = write_fatal};
    sigemptyset(&action.sa_mask);
    sigaction(SIGILL, &action, NULL);
    sigaction(SIGBUS, &action, NULL);
    sigaction(SIGSEGV, &action, NULL);
    sigaction(SIGABRT, &action, NULL);
    sigaction(SIGFPE, &action, NULL);
    fprintf(stderr, "\nq2-boy 0.2.2, engine=%s\n",
            use_dynarec ? "dynamic recompiler (opt-in)" : "interpreter");
    FILE *memory = fopen("/proc/meminfo", "r");
    if (memory) {
        char line[128];
        for (int i = 0; i < 4 && fgets(line, sizeof line, memory); ++i) fputs(line, stderr);
        fclose(memory);
    }
}
int main(int argc, char **argv) {
    smoke = argc >= 4 && (!strcmp(argv[1], "--smoke") || !strcmp(argv[1], "--smoke-dynarec"));
    use_dynarec = smoke && !strcmp(argv[1], "--smoke-dynarec");
    if (smoke) data_dir = argv[3];
    else {
        /* Move existing data only when the new data directory does not exist. */
        struct stat legacy, current;
        const char *legacy_dir = "/mnt/mmc/GBA";
        if (stat(data_dir, &current) && errno == ENOENT &&
            !stat(legacy_dir, &legacy) && S_ISDIR(legacy.st_mode) &&
            rename(legacy_dir, data_dir)) {
            fprintf(stderr, "Could not move existing cartridge data: %s\n", strerror(errno));
            return 1;
        }
    }
    if (!prepare_dirs()) return 1;
    if (!smoke) {
        log_launch();
        const char *bridge = getenv("Q2_BOY_AUDIO_BRIDGE");
        audio_bridge = bridge != NULL;
        native_supported = bridge && *bridge == '2';
        music_mode = audio_bridge && argc == 1;
        audio_route = argc > 1 && !strcmp(argv[1], "plug:bluealsa") ? 1 :
                      argc > 1 && !strcmp(argv[1], "usb") ? (argc > 2 && argv[2][0] == 'h' ? 3 : 2) : 0;
        if (!platform_open(argc > 1 ? argv[1] : NULL, argc > 2 ? argv[2] : NULL, music_mode)) {
            fprintf(stderr, "Framebuffer or input socket initialization failed: %s\n", strerror(errno));
            platform_close();
            return 1;
        }
    }
    load_config();
    locate_app(argv[0]);
    initialize_palette();
    if (smoke) {
        int result = smoke_test(argv[2]);
        for (int i = 4; !result && i < argc; ++i) result = smoke_test(argv[i]);
        release_core();
        if (gambatte_library) dlclose(gambatte_library);
        return result;
    }
    if (!raw_buttons && !native_supported) status("Limited input: holding Up may exit");
    scan_roms();
    uint64_t next_frame = 0;
    while (!quitting) {
        Event e;
        for (int i = 0; i < 128 && platform_event(&e); ++i) if (!native_yielded) read_event(e);
        uint64_t now = clock_ns();
        audio_poll(now);
        now = clock_ns();
        if (notice_end && now >= notice_end) { notice_end = 0; dirty = 1; }
        if (screen == GAME && now >= next_frame) {
            uint64_t period = (uint64_t)(1000000000.0 / (fps * (fast ? fast_rate : 1)));
            if (!next_frame || now - next_frame > period * 3) next_frame = now;
            next_frame += period;
            launch_stage = have_frame ? 4 : 3;
            if (!speed_start) { speed_start = now; speed_frames = video_frames = 0; }
            if (now - speed_start >= (fast ? 2000000000ull : 5000000000ull)) {
                double measured = speed_frames * 1000000000.0 / (now - speed_start);
                if (fast) { measured_speed = measured / fps; dirty = 1; }
                double pictures = video_frames * 1000000000.0 / (now - speed_start);
                fprintf(stderr, "%s (target %dx): %.1f emulated frames/s (%.2fx), %.1f pictures/s\n",
                        fast ? "Fast-forward" : "Normal speed", fast ? fast_rate : 1,
                        measured, measured / fps, pictures);
                speed_start = now;
                speed_frames = video_frames = 0;
            }
            schedule_video(now);
            core->run();
            ++speed_frames;
            pressed_this_frame = 0;
        } else if (screen != GAME) next_frame = 0;
        if (running && now >= next_save) {
            cartridge_saves(true);
            next_save = now + 20000000000ull;
        }
        if (dirty && !native_yielded) draw_ui();
        if (screen != GAME || clock_ns() < next_frame) usleep(1000);
    }
    close_rom();
    if (gambatte_library) dlclose(gambatte_library);
    platform_close();
    fprintf(stderr, "Exited cleanly\n");
    return 0;
}
