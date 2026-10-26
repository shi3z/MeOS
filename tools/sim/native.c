/* Host application simulator, not an ESP32 instruction/peripheral emulator.
 * The actual Scheme IDE, DQ widgets, fonts, HID decoder and IME are linked in.
 * All SDL and application work runs on the main thread. */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE
#include "../../drivers/lcd/fbcon.h"
#include "../../drivers/lcd/font8x8.h"
#include "../../drivers/lcd/font_ja16.h"
#include "../../drivers/usbkbd/hid_keys.h"
#include "../../user/home/home.h"
#include "../../user/scheme/ide.h"
#include "replay.h"
#include "../../user/write/write.h"
#include <SDL.h>
#include <errno.h>
#include <fcntl.h>
#include <m5os/display.h>
#include <m5os/errno.h>
#include <m5os/ime.h>
#include <m5os/kmalloc.h>
#include <m5os/kprintf.h>
#include <m5os/task.h>
#include <m5os/vfs.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define WIDTH 1280
#define HEIGHT 720
#define INPUT_CAP 8192
#define HEAP_CAP (28u * 1024 * 1024)
uint32_t arch_irq_disable(void) { return 0; }
void arch_irq_restore(uint32_t state) { (void)state; }
static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static uint16_t pixels[WIDTH * HEIGHT], *target = pixels;
static bool dirty, paused, smoke, headless, closing, pumping, app_drawn;
static unsigned vfs_open_count;
static unsigned char input[INPUT_CAP];
static size_t input_head, input_count, heap_used;
static int root_fd = -1;
static const char *shots;
static unsigned lines_drawn, ime_draws;
static uint64_t last_present, deadline;
static unsigned eval_poll_delay;
static sim_replay replay;
static bool replaying, replay_started;
static size_t replay_step;
static uint64_t replay_due, record_time;
static FILE *record_file;
static unsigned manual_shot, record_count;
static uint64_t record_duration;
static int mouse_x, mouse_y;
static bool mouse_down;
static void die(const char *);
#define SYNTHETIC_KEY 0x4d354f53u
static unsigned record_delay(void)
{
    uint64_t now = SDL_GetTicks64();
    if (!record_time)
        record_time = now;
    uint64_t delay = now - record_time;
    if (delay > 60000)
        die("recording pause exceeds 60 seconds");
    if (++record_count > SIM_ACTION_MAX || record_duration + delay > SIM_SCRIPT_MAX_MS)
        die("recording limit exceeded");
    record_duration += delay;
    record_time = now;
    return (unsigned)delay;
}

static void die(const char *why)
{
    fprintf(stderr, "sim: %s\n", why);
    exit(1);
}
static void present(void)
{
    if (!dirty || !texture)
        return;
    SDL_UpdateTexture(texture, NULL, pixels, WIDTH * (int)sizeof(uint16_t));
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);
    dirty = false;
    last_present = SDL_GetTicks64();
}
static void pixel(int x, int y, uint16_t color)
{
    if (x >= 0 && y >= 0 && x < WIDTH && y < HEIGHT) {
        target[y * WIDTH + x] = color;
        dirty = true;
    }
}
void display_fill_rect(int x, int y, int w, int h, uint16_t color)
{
    for (int row = y < 0 ? 0 : y; row < y + h && row < HEIGHT; row++)
        for (int col = x < 0 ? 0 : x; col < x + w && col < WIDTH; col++)
            pixel(col, row, color);
}
void display_clear(uint16_t color) { display_fill_rect(0, 0, WIDTH, HEIGHT, color); }
void display_draw_line(int x0, int y0, int x1, int y1, uint16_t color)
{
    lines_drawn++;
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, error = dx + dy;
    for (;;) {
        pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1)
            break;
        int e = 2 * error;
        if (e >= dy) {
            error += dy;
            x0 += sx;
        }
        if (e <= dx) {
            error += dx;
            y0 += sy;
        }
    }
}
void display_draw_rect(int x, int y, int w, int h, uint16_t color)
{
    display_fill_rect(x, y, w, 1, color);
    display_fill_rect(x, y + h - 1, w, 1, color);
    display_fill_rect(x, y, 1, h, color);
    display_fill_rect(x + w - 1, y, 1, h, color);
}
static void glyph(int x, int y, uint32_t cp, uint16_t fg, uint16_t bg, int scale)
{
    int size = 8 * scale;
    if (cp < 128) {
        if (cp < FONT8X8_FIRST || cp > FONT8X8_LAST)
            cp = ' ';
        for (int row = 0; row < size; row++)
            for (int col = 0; col < size; col++)
                pixel(x + col, y + row,
                      font8x8[cp - FONT8X8_FIRST][row / scale] & (1u << (col / scale)) ? fg : bg);
    } else {
        uint16_t rows[16];
        if (cp > 65535 || !font_ja16_get_glyph((uint16_t)cp, rows, NULL)) {
            memset(rows, 0, sizeof rows);
            rows[2] = rows[13] = 0x3ffc;
            for (int n = 3; n < 13; n++)
                rows[n] = 0x2004;
        }
        for (int row = 0; row < size; row++)
            for (int col = 0; col < size; col++)
                pixel(x + col, y + row,
                      rows[row * 16 / size] & (0x8000u >> (col * 16 / size)) ? fg : bg);
    }
}
void display_draw_char(int x, int y, char c, uint16_t fg, uint16_t bg, int scale)
{
    if (scale > 0)
        glyph(x, y, (unsigned char)c, fg, bg, scale);
}
void display_draw_text(int x, int y, const char *text, uint16_t fg, uint16_t bg, int scale)
{
    if (scale < 1)
        return;
    app_drawn = true;
    const unsigned char *p = (const unsigned char *)text;
    while (*p && x + 8 * scale <= WIDTH) {
        uint32_t cp = *p++;
        unsigned n = 0;
        if (cp >= 0xf0) {
            cp &= 7;
            n = 3;
        } else if (cp >= 0xe0) {
            cp &= 15;
            n = 2;
        } else if (cp >= 0xc0) {
            cp &= 31;
            n = 1;
        }
        while (n-- && *p && (*p & 0xc0) == 0x80)
            cp = (cp << 6) | (*p++ & 63);
        glyph(x, y, cp, fg, bg, scale);
        x += 8 * scale;
    }
}
void display_draw_codepoints(int x, int y, const uint16_t *cp, int n, uint16_t fg, uint16_t bg,
                             int scale)
{
    ime_draws++;
    for (int k = 0; k < n; k++)
        glyph(x + k * 8 * scale, y, cp[k], fg, bg, scale);
}
void display_draw_bitmap(int x, int y, int w, int h, const uint16_t *data)
{
    app_drawn = true;
    for (int row = 0; row < h; row++)
        for (int col = 0; col < w; col++)
            pixel(x + col, y + row, data[row * w + col]);
}
void display_read_bitmap(int x, int y, int w, int h, uint16_t *data)
{
    for (int row = 0; row < h; row++)
        for (int col = 0; col < w; col++) {
            int xx = x + col, yy = y + row;
            data[row * w + col] =
                xx >= 0 && yy >= 0 && xx < WIDTH && yy < HEIGHT ? pixels[yy * WIDTH + xx] : 0;
        }
}
bool display_is_ready(void) { return true; }
void display_set_target(uint16_t *buffer) { target = buffer ? buffer : pixels; }
void display_present(int x, int y, int w, int h) {
    for(int row=y;row<y+h && row<HEIGHT;row++)
        for(int col=x;col<x+w && col<WIDTH;col++)
            if(row>=0 && col>=0)pixels[row*WIDTH+col]=target[row*WIDTH+col];
    dirty=true;app_drawn=true;
}
/* Probe the production display lease after each app exits. */
static bool display_free(void)
{
    static const char probe;
    if (!display_try_claim(&probe)) return false;
    display_release_claim(&probe);
    return true;
}
bool fbcon_is_paused(void) { return paused; }
void fbcon_pause(bool value) { paused = value; }
int fbcon_cell_px(void) { return 16; }
void fbcon_overlay_colors(int style, uint16_t *fg, uint16_t *bg)
{
    *fg = COLOR_WHITE;
    *bg = COLOR_BLACK;
    if (style == FBCON_OVL_PREEDIT || style == FBCON_OVL_PENDING)
        *fg = COLOR_CYAN;
    if (style == FBCON_OVL_FOCUS || style == FBCON_OVL_CAND_SEL) {
        *fg = COLOR_BLACK;
        *bg = COLOR_CYAN;
    }
}
/* Paused apps use the real IME's saved-pixel inline renderer, not fbcon's
 * console overlay. These hooks are intentionally inert for this target. */
void fbcon_get_geometry(int *r, int *c, int *cr, int *cc)
{
    *r = 45;
    *c = 80;
    *cr = *cc = 0;
}
void fbcon_overlay_begin(void) {}
void fbcon_overlay_end(void) {}
void fbcon_overlay_text(int a, int r, int c, const uint16_t *p, int n, int s)
{
    (void)a;
    (void)r;
    (void)c;
    (void)p;
    (void)n;
    (void)s;
}
void ime_dict_load_async(void) {} /* Use the real built-in SKK dictionary. */

typedef union {
    max_align_t alignment;
    size_t size;
} allocation;
void *kmalloc_psram(size_t n)
{
    if (n > HEAP_CAP - heap_used)
        return NULL;
    allocation *p = malloc(sizeof *p + n);
    if (!p)
        return NULL;
    p->size = n;
    heap_used += n;
    return p + 1;
}
size_t sdroot_asset_cache_trim(void) { return 0; }
void kfree(void *data)
{
    if (data) {
        allocation *p = (allocation *)data - 1;
        heap_used -= p->size;
        free(p);
    }
}
int ksnprintf(char *b, size_t n, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int result = vsnprintf(b, n, fmt, args);
    va_end(args);
    return result < 0 ? 0 : (size_t)result >= n ? (int)(n ? n - 1 : 0) : result;
}
void kprintf(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vfprintf(stdout, fmt, args);
    va_end(args);
}
size_t kconsole_inject_n(const char *s, size_t n)
{
    size_t done = 0;
    while (done < n && input_count < INPUT_CAP) {
        input[(input_head + input_count) % INPUT_CAP] = (unsigned char)s[done++];
        input_count++;
    }
    return done;
}
static SDL_Keymod sdl_modifiers(uint8_t modifiers, bool caps)
{
    return (modifiers & HID_MOD_LCTRL ? KMOD_LCTRL : 0) |
           (modifiers & HID_MOD_LSHIFT ? KMOD_LSHIFT : 0) |
           (modifiers & HID_MOD_LALT ? KMOD_LALT : 0) |
           (modifiers & HID_MOD_RCTRL ? KMOD_RCTRL : 0) |
           (modifiers & HID_MOD_RSHIFT ? KMOD_RSHIFT : 0) | (modifiers & 0x40 ? KMOD_RALT : 0) |
           (caps ? KMOD_CAPS : 0);
}
static void queue_hid_full(uint8_t usage, uint8_t modifiers, bool caps)
{
    /* SDL2-compat does not preserve SDL_Keysym.unused. A user event carries
     * explicit synthetic provenance and is translated into the same key path. */
    SDL_Event event = {0};
    event.type = SDL_USEREVENT;
    event.user.code = SYNTHETIC_KEY;
    event.user.data1 =
        (void *)(uintptr_t)(usage | ((unsigned)modifiers << 8) | ((unsigned)caps << 16));
    if (SDL_PushEvent(&event) != 1)
        die("cannot queue keyboard event");
}
static void queue_hid(uint8_t usage, uint8_t modifiers) { queue_hid_full(usage, modifiers, false); }
/* Invert the shared HID decoder for scripted ASCII keys. Scripted and real
 * keyboard events then take exactly the same SDL -> HID -> IME -> app path. */
static void feed_text(const char *s)
{
    while (*s) {
        bool found = false;
        const uint8_t modifiers[] = {0, HID_MOD_LSHIFT, HID_MOD_LCTRL};
        for (unsigned m = 0; m < sizeof modifiers && !found; m++) {
            for (uint8_t usage = 4; usage <= 0x45; usage++) {
                char decoded[8];
                hid_decode(usage, modifiers[m], false, decoded);
                if (decoded[0] == *s && !decoded[1]) {
                    queue_hid(usage, modifiers[m]);
                    found = true;
                    break;
                }
            }
        }
        if (!found)
            die("script contains an unsupported ASCII key");
        s++;
    }
}
static void snapshot(const char *name)
{
    if (!sim_shot_name(name))
        die("invalid screenshot name");
    if (record_file) {
        fprintf(record_file, "shot %u %s\n", record_delay(), name);
        if (fflush(record_file))
            die("cannot write recording");
    }
    if (!shots)
        return;
    char path[1024];
    if (snprintf(path, sizeof path, "%s/%s.bmp", shots, name) >= (int)sizeof path)
        die("screenshot path too long");
    SDL_Surface *surface =
        SDL_CreateRGBSurfaceWithFormat(0, WIDTH, HEIGHT, 16, SDL_PIXELFORMAT_RGB565);
    if (!surface)
        die(SDL_GetError());
    for (int row = 0; row < HEIGHT; row++)
        memcpy((unsigned char *)surface->pixels + row * surface->pitch, pixels + row * WIDTH,
               WIDTH * 2);
    if (!surface || SDL_SaveBMP(surface, path) < 0)
        die(SDL_GetError());
    SDL_FreeSurface(surface);
    printf("screenshot: %s\n", path);
}
typedef struct {
    const char *keys, *text, *shot;
} action;
static const action smoke_actions[] = {{.shot = "initial"},
                                       {.text = "\x18\x02"},
                                       {.shot = "graph"},
                                       {.text = "; "},
                                       {.keys = IME_TOGGLE_KEY},
                                       {.text = "nihongo"},
                                       {.keys = " "},
                                       {.keys = " "},
                                       {.keys = "1"},
                                       {.shot = "ime-candidates"},
                                       {.keys = "\r"},
                                       {.keys = IME_TOGGLE_KEY},
                                       {.keys = "\r"},
                                       {.text = "\x18\x13"},
                                       {.text = "\x18\x02"},
                                       {.shot = "final"},
                                       {.text = "\x11"}};
static unsigned smoke_step;
static uint64_t next_action;
static void pump(bool idle)
{
    if (pumping)
        return;
    pumping = true;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        bool synthetic = event.type == SDL_USEREVENT && event.user.code == SYNTHETIC_KEY;
        if (synthetic) {
            unsigned data = (unsigned)(uintptr_t)event.user.data1;
            memset(&event, 0, sizeof event);
            event.type = SDL_KEYDOWN;
            event.key.keysym.scancode = (SDL_Scancode)(data & 255);
            event.key.keysym.mod = sdl_modifiers((uint8_t)(data >> 8), (data >> 16) != 0);
        }
        if (event.type == SDL_QUIT) {
            if (replaying)
                die("replay interrupted by window close");
            closing = true;
        } else if (event.type == SDL_KEYDOWN) {
            if (replaying && !synthetic)
                continue;
            if (event.key.keysym.scancode < 4 || event.key.keysym.scancode > 255)
                continue;
            if (event.key.keysym.scancode == SDL_SCANCODE_F12) {
                char name[48];
                snprintf(name, sizeof name, "capture-%ld-%04u", (long)getpid(), ++manual_shot);
                snapshot(name);
                continue;
            }
            SDL_Keymod mod = event.key.keysym.mod;
            uint8_t modifiers =
                (mod & KMOD_LCTRL ? HID_MOD_LCTRL : 0) | (mod & KMOD_LSHIFT ? HID_MOD_LSHIFT : 0) |
                (mod & KMOD_LALT ? HID_MOD_LALT : 0) | (mod & KMOD_RCTRL ? HID_MOD_RCTRL : 0) |
                (mod & KMOD_RSHIFT ? HID_MOD_RSHIFT : 0) | (mod & KMOD_RALT ? 0x40 : 0);
            if (record_file) {
                fprintf(record_file, "key %u %u %u %u\n", record_delay(),
                        (unsigned)event.key.keysym.scancode, modifiers, (mod & KMOD_CAPS) != 0);
                if (fflush(record_file))
                    die("cannot write recording");
            }
            /* SDL scancodes follow USB keyboard HID usage IDs for these keys. */
            char bytes[8];
            hid_decode((uint8_t)event.key.keysym.scancode, modifiers, (mod & KMOD_CAPS) != 0,
                       bytes);
            if (bytes[0])
                ime_feed(bytes);
        } else if (event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_MOUSEBUTTONUP ||
                   event.type == SDL_MOUSEMOTION) {
            float x, y;
            int window_x = event.type == SDL_MOUSEMOTION ? event.motion.x : event.button.x;
            int window_y = event.type == SDL_MOUSEMOTION ? event.motion.y : event.button.y;
            SDL_RenderWindowToLogical(renderer, window_x, window_y, &x, &y);
            mouse_x = (int)x;
            mouse_y = (int)y;
            if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT)
                mouse_down = true;
            else if (event.type == SDL_MOUSEBUTTONUP && event.button.button == SDL_BUTTON_LEFT)
                mouse_down = false;
        } else if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_EXPOSED)
            dirty = true;
    }
    uint64_t now = SDL_GetTicks64();
    if (smoke && app_drawn && !input_count && now >= next_action &&
        smoke_step < sizeof smoke_actions / sizeof smoke_actions[0] &&
        (!smoke_actions[smoke_step].shot || idle)) {
        const action *a = &smoke_actions[smoke_step++];
        if (a->keys) {
            if (!strcmp(a->keys, IME_TOGGLE_KEY))
                queue_hid(0x2c, HID_MOD_LCTRL);
            else
                feed_text(a->keys);
        }
        if (a->text)
            feed_text(a->text);
        if (a->shot)
            snapshot(a->shot);
        next_action = now + 80;
    }
    if (replaying && app_drawn && !replay_started) {
        replay_started = true;
        replay_due = now + replay.actions[0].delay;
        deadline = now + replay.duration + 5000;
    }
    if (replaying && replay_started && !input_count && now >= replay_due &&
        replay_step < replay.count && (!replay.actions[replay_step].shot[0] || idle)) {
        const sim_action *a = &replay.actions[replay_step++];
        if (a->shot[0])
            snapshot(a->shot);
        else
            queue_hid_full(a->usage, a->modifiers, a->caps);
        if (replay_step < replay.count)
            replay_due = now + replay.actions[replay_step].delay;
    }
    if (closing && !input_count) {
        feed_text("\x11");
        closing = false; /* Leave unsaved-source confirmation to the user. */
    }
    if ((smoke || (replaying && replay_started)) && now > deadline)
        die("simulation timed out");
    if (now - last_present >= 16)
        present();
    pumping = false;
}

static bool home_touch(int *x, int *y, void *context)
{
    (void)context;
    *x = mouse_x;
    *y = mouse_y;
    return mouse_down;
}

int kconsole_getc(void)
{
    pump(false);
    if (!input_count)
        return -1;
    int c = input[input_head];
    input_head = (input_head + 1) % INPUT_CAP;
    input_count--;
    return c;
}
void task_sleep(uint32_t ms)
{
    /* Scheme cancellation polls use sleep(0) while evaluation/drawing is still
     * in progress. Screenshots must wait for a real input-idle boundary. */
    pump(ms > 0);
    present();
    if (ms || eval_poll_delay)
        SDL_Delay(ms ? ms : eval_poll_delay);
}

static int host_error(void)
{
    if (errno == ENOENT)
        return ERR_NOENT;
    if (errno == ENOTDIR)
        return ERR_NOTDIR;
    if (errno == EACCES || errno == EPERM || errno == ELOOP)
        return ERR_PERM;
    if (errno == ENOSPC)
        return ERR_NOSPC;
    return ERR_IO;
}
int vfs_normalize(const char *path, char *out)
{
    if (path[0] != '/' || strlen(path) >= VFS_PATH_MAX)
        return ERR_INVAL;
    char copy[VFS_PATH_MAX];
    strcpy(copy, path);
    size_t n = 1;
    out[0] = '/';
    out[1] = 0;
    char *save = NULL;
    for (char *p = strtok_r(copy, "/", &save); p; p = strtok_r(NULL, "/", &save)) {
        if (!strcmp(p, "."))
            continue;
        if (!strcmp(p, "..")) {
            if (n == 1)
                return ERR_PERM;
            while (n > 1 && out[n - 1] != '/')
                n--;
            if (n > 1)
                n--;
            out[n] = 0;
            continue;
        }
        size_t len = strlen(p);
        if (n > 1)
            out[n++] = '/';
        memcpy(out + n, p, len + 1);
        n += len;
    }
    return 0;
}
/* Walk directory descriptors, refusing symlinks at every step. MeOS paths
 * cannot escape the per-process host workspace, including via symlink tricks. */
static int parent(const char *path, char *name)
{
    char normalized[VFS_PATH_MAX];
    int rc = vfs_normalize(path, normalized);
    if (rc < 0)
        return rc;
    if (!strcmp(normalized, "/"))
        return ERR_INVAL;
    int fd = dup(root_fd);
    if (fd < 0)
        return host_error();
    char *save = NULL, *p = strtok_r(normalized + 1, "/", &save);
    while (p) {
        char *next = strtok_r(NULL, "/", &save);
        if (!next) {
            strcpy(name, p);
            return fd;
        }
        int child = openat(fd, p, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
        int error = host_error();
        close(fd);
        if (child < 0)
            return error;
        fd = child;
        p = next;
    }
    close(fd);
    return ERR_INVAL;
}
int vfs_open(const char *path, uint32_t flags)
{
    char name[VFS_PATH_MAX];
    int dir = parent(path, name);
    if (dir < 0)
        return dir;
    int mode = flags & VFS_O_WRITE ? O_WRONLY : O_RDONLY;
    if (flags & VFS_O_CREATE)
        mode |= O_CREAT;
    if (flags & VFS_O_TRUNC)
        mode |= O_TRUNC;
    if (flags & VFS_O_APPEND)
        mode |= O_APPEND;
    int fd = openat(dir, name, mode | O_NOFOLLOW | O_NONBLOCK, 0600);
    int error = host_error();
    close(dir);
    if (fd < 0)
        return error;
    struct stat info;
    if (fstat(fd, &info) < 0 || !S_ISREG(info.st_mode)) {
        close(fd);
        return ERR_INVAL;
    }
    vfs_open_count++;
    return fd;
}
int vfs_stat(const char *path, vfs_info_t *out)
{
    char name[VFS_PATH_MAX];
    int dir = parent(path, name);
    if (dir < 0)
        return dir;
    struct stat info;
    int rc = fstatat(dir, name, &info, AT_SYMLINK_NOFOLLOW);
    int error = host_error();
    close(dir);
    if (rc < 0)
        return error;
    if (S_ISLNK(info.st_mode))
        return ERR_PERM;
    memset(out, 0, sizeof *out);
    out->type = S_ISDIR(info.st_mode) ? FS_TYPE_DIR : FS_TYPE_FILE;
    out->size = info.st_size > UINT32_MAX ? UINT32_MAX : (uint32_t)info.st_size;
    return 0;
}
int vfs_read(int fd, void *data, size_t n)
{
    ssize_t rc;
    do
        rc = read(fd, data, n);
    while (rc < 0 && errno == EINTR);
    return rc < 0 ? host_error() : (int)rc;
}
int vfs_write(int fd, const void *data, size_t n)
{
    ssize_t rc;
    do
        rc = write(fd, data, n);
    while (rc < 0 && errno == EINTR);
    return rc < 0 ? host_error() : (int)rc;
}
int vfs_close(int fd)
{
    if (close(fd) < 0) return host_error();
    if (!vfs_open_count) die("unbalanced VFS close");
    vfs_open_count--;
    return 0;
}

static void load_replay(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) die("cannot open replay file");
    char error[160] = "cannot close replay file";
    bool ok = sim_replay_load(f, &replay, error, sizeof error);
    if (fclose(f)) ok = false;
    if (!ok) die(error);
    for (size_t n = 0; n < replay.count; n++)
        if (replay.actions[n].shot[0] && !shots)
            die("replay screenshots require --screenshots DIR");
    replaying = true;
    replay_started = app_drawn = closing = false;
    replay_step = 0;
}


int main(int argc, char **argv)
{
    const char *workspace = NULL, *source = NULL, *record_path = NULL, *replay_path = NULL;
    char normalized_source[VFS_PATH_MAX];
    bool writing = false, homing = false;
    struct { bool writing; const char *source, *keys; } stages[16];
    unsigned stage_count = 0;
    for (int n = 1; n < argc; n++) {
        if (!strcmp(argv[n], "--stage") && n + 3 < argc) {
            if (stage_count == 16) die("at most 16 stages are allowed");
            const char *app = argv[++n];
            if (strcmp(app, "scheme") && strcmp(app, "write")) die("stage app must be scheme or write");
            stages[stage_count].writing = !strcmp(app, "write");
            stages[stage_count].source = argv[++n];
            stages[stage_count++].keys = argv[++n];
        } else if (!strcmp(argv[n], "--write"))
            writing = true;
        else if (!strcmp(argv[n], "--home"))
            homing = true;
        else if (!strcmp(argv[n], "--smoke"))
            smoke = true;
        else if (!strcmp(argv[n], "--headless"))
            headless = true;
        else if (!strcmp(argv[n], "--workspace") && n + 1 < argc)
            workspace = argv[++n];
        else if (!strcmp(argv[n], "--screenshots") && n + 1 < argc)
            shots = argv[++n];
        else if (!strcmp(argv[n], "--record") && n + 1 < argc)
            record_path = argv[++n];
        else if (!strcmp(argv[n], "--replay") && n + 1 < argc)
            replay_path = argv[++n];
        else if (!strcmp(argv[n], "--source") && n + 1 < argc)
            source = argv[++n];
        else
            die("usage: meos-sim --workspace DIR [--smoke] [--headless] [--screenshots DIR] "
                "[--home | --write] [--source /home/FILE] [--record NEW_FILE | --replay FILE] "
                "[--stage scheme|write /home/FILE REPLAY ...]");
    }
    if (!workspace || (smoke && (source || writing || homing)) || (homing && (writing || source)))
        die("workspace required; smoke uses the built-in demo");
    if (stage_count && (writing || homing || source || smoke || record_path || replay_path))
        die("stages cannot be combined with single-app options");
    if ((record_path && replay_path) || (smoke && replay_path))
        die("record/replay and smoke/replay are exclusive");
    if (replay_path) load_replay(replay_path);
    if (record_path) {
        record_file = fopen(record_path, "wx");
        if (!record_file)
            die("record path must be a new writable file");
        fputs("# MeOS HID recording v1\n", record_file);
    }
    if (source) {
        if (vfs_normalize(source, normalized_source) < 0)
            die("invalid source VFS path");
        source = normalized_source;
    }
    const char *slow_eval = getenv("M5OS_SIM_EVAL_POLL_DELAY_MS");
    if (slow_eval) {
        char *end;
        unsigned long delay = strtoul(slow_eval, &end, 10);
        if (!*slow_eval || *end || delay > 20) die("invalid evaluation poll delay");
        eval_poll_delay = (unsigned)delay;
    }
    root_fd = open(workspace, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (root_fd < 0)
        die("cannot open workspace directory");
    if (mkdirat(root_fd, "home", 0700) < 0 && errno != EEXIST)
        die("cannot create home");
    if (headless)
        SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) < 0)
        die(SDL_GetError());
    window = SDL_CreateWindow("MeOS App Simulator — Tab5 / Scheme / Ctrl-Space: Japanese",
                              SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, WIDTH, HEIGHT,
                              SDL_WINDOW_RESIZABLE | (headless ? SDL_WINDOW_HIDDEN : 0));
    if (!window)
        die(SDL_GetError());
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer)
        die(SDL_GetError());
    SDL_RenderSetLogicalSize(renderer, WIDTH, HEIGHT);
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING,
                                WIDTH, HEIGHT);
    if (!texture)
        die(SDL_GetError());
    /* Use physical key/HID events and MeOS's own IME, not the host text IME. */
    SDL_StopTextInput();
    ime_init();
    deadline = SDL_GetTicks64() + 10000;
    printf("MeOS application simulator; workspace=%s\n", workspace);
    fflush(stdout);
    bool app_ok = true;
    if (stage_count) {
        size_t baseline = 0;
        for (unsigned n = 0; n < stage_count; n++) {
            if (vfs_normalize(stages[n].source, normalized_source) < 0) die("invalid stage source");
            unsigned before_lines = lines_drawn;
            load_replay(stages[n].keys);
            bool ok = stages[n].writing ? write_file(normalized_source) : scheme_ide(normalized_source);
            present();
            if (!ok || replay_step != replay.count || input_count || !display_free() || paused ||
                target != pixels || vfs_open_count)
                die("stage failed or left display/input/files in use");
            if (!n) baseline = heap_used;
            else if (heap_used != baseline) die("stage retained app heap allocations");
            printf("STAGE PASS: %u %s; heap=%zu; lines=%u; display/input/files released\n",
                   n + 1, stages[n].writing ? "write" : "scheme", heap_used, lines_drawn - before_lines);
        }
    } else {
        if (homing) {
            int selected = 0;
            home_action_t action = home_select(&selected, home_touch, NULL);
            app_ok = action == HOME_SHELL;
            printf("HOME PASS: selected %s; keyboard/mouse paths active\n", home_item_label(selected));
        } else
            app_ok = writing ? write_file(source ? source : "/home/note.txt") : scheme_ide(source);
    }
    present();
    if (smoke) {
        char saved[1024] = {0};
        int fd = vfs_open("/home/workspace.scm", VFS_O_READ);
        int n = fd >= 0 ? vfs_read(fd, saved, sizeof saved - 1) : -1;
        if (fd >= 0)
            vfs_close(fd);
        if (n < 0 || !strstr(saved, "; 日本語\n") ||
            lines_drawn < 100 || smoke_step != sizeof smoke_actions / sizeof smoke_actions[0] ||
            !display_free() || paused) {
            fprintf(stderr,
                    "smoke state: read=%d IME draws=%u lines=%u step=%u owner=%p "
                    "paused=%d\n",
                    n, ime_draws, lines_drawn, smoke_step, (void *)(uintptr_t)!display_free(),
                    paused);
            die("smoke failed: graph, IME, source save or cleanup incomplete");
        }
        printf("SIM PASS: graph + actual MeOS IME conversion + Japanese source save + cleanup\n");
    }
    if (replaying) {
        if (replay_step != replay.count || input_count || !display_free() || paused)
            die("replay ended before all actions completed");
        printf("REPLAY PASS: %zu actions completed\n", replay.count);
    }
    if (record_file && fclose(record_file))
        die("cannot close recording");
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    close(root_fd);
    return app_ok ? 0 : 1;
}

int vfs_replace_file(const char *path,const void *data,size_t len,size_t *written)
{
    return vfs_write_file(path,data,len,written);
}
