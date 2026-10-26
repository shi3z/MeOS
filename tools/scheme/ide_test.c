/* Scripted integration of the real IDE with fake display/console/VFS devices.
 * This verifies application behaviour; it does not qualify the physical IME. */
#include "../../user/scheme/ide.c"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const unsigned char *keys;
static bool paused, claimed, confirm = true, fail_close, fail_read, fail_write;
static int allocations, anchors, releases, graph_lines, last_z_x = -1;
static unsigned idle;
static char file_data[SCHEME_SOURCE_CAP + 2];
static size_t file_len, file_pos;
static bool exists = true;
static void script(const char *s)
{
    keys = (const unsigned char *)s;
    idle = 0;
}
int kconsole_getc(void) { return keys && *keys ? *keys++ : -1; }
void task_sleep(uint32_t ms)
{
    (void)ms;
    assert(++idle < 10000);
}
void *kmalloc_psram(size_t n)
{
    void *p = malloc(n);
    if (p)
        allocations++;
    return p;
}
size_t sdroot_asset_cache_trim(void) { return 0; }
void kfree(void *p)
{
    if (p)
        allocations--;
    free(p);
}
int ksnprintf(char *b, size_t n, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int rc = vsnprintf(b, n, fmt, args);
    va_end(args);
    return rc < 0 ? 0 : (size_t)rc >= n ? (int)(n ? n - 1 : 0) : rc;
}
void kprintf(const char *fmt, ...) { (void)fmt; }
bool display_is_ready(void) { return true; }
bool display_try_claim(const void *owner)
{
    (void)owner;
    if (claimed)
        return false;
    claimed = true;
    return true;
}
void display_release_claim(const void *owner)
{
    (void)owner;
    assert(claimed);
    claimed = false;
    releases++;
}
void display_set_target(uint16_t *p) { (void)p; }
void display_present(int x, int y, int w, int h)
{
    assert(x >= 0 && y >= 0 && x + w <= 1280 && y + h <= 720);
}
bool fbcon_is_paused(void) { return paused; }
void fbcon_pause(bool value) { paused = value; }
int fbcon_cell_px(void) { return 16; }
int dq_cols(void) { return 80; }
int dq_rows(void) { return 45; }
void dq_clear(void) {}
void dq_text(int x, int y, const char *s)
{
    assert(x >= 0 && x < 80 && y >= 0 && y < 45);
    assert(scheme_utf8_text(s, strlen(s)));
}
void dq_text_rev(int x, int y, const char *s) { dq_text(x, y, s); }
void dq_cursor(int x, int y, bool on)
{
    (void)on;
    assert(x >= 0 && x < 80 && y >= 0 && y < 45);
}
void dq_message(const char *a, const char *b)
{
    (void)a;
    (void)b;
}
bool dq_confirm(const char *s)
{
    (void)s;
    return confirm;
}
void display_fill_rect(int x, int y, int w, int h, uint16_t c)
{
    (void)c;
    assert(x >= 0 && y >= 0 && x + w <= 1280 && y + h <= 720);
}
void display_draw_rect(int x, int y, int w, int h, uint16_t c) { display_fill_rect(x, y, w, h, c); }
void display_draw_line(int a, int b, int c, int d, uint16_t color)
{
    (void)color;
    assert(a >= 0 && a < 1280 && c >= 0 && c < 1280 && b >= 0 && b < 720 && d >= 0 && d < 720);
    graph_lines++;
}
void ime_clear_anchor(void) {}
void ime_set_app_view(bool enabled) { (void)enabled; }
size_t ime_take_commit(char *out, size_t capacity)
{
    (void)out;
    (void)capacity;
    return 0;
}
bool ime_read_view(ime_view *view)
{
    memset(view, 0, sizeof *view);
    return true;
}
void ime_set_anchor(int x, int y)
{
    assert(x >= 0 && x < 80 && y >= 0 && y < 45);
    anchors++;
}
const char *ime_status(void) { return "hiragana"; }
bool bizud_begin(void) { return true; }
void bizud_end(void) {}
size_t bizud_cache_bytes(void) { return 0; }
unsigned bizud_cache_users(void) { return 0; }
size_t bizud_trim(void) { return 0; }
uint32_t bizud_decode(const char **text)
{
    const unsigned char *p = (const unsigned char *)*text;
    uint32_t cp = *p++;
    unsigned n = cp >= 0xf0 ? 3 : cp >= 0xe0 ? 2 : cp >= 0xc2 ? 1 : 0;
    if (n) {
        cp &= (1u << (6 - n)) - 1;
        while (n--) cp = (cp << 6) | (*p++ & 63);
    }
    *text = (const char *)p;
    return cp;
}
int bizud_width(uint32_t cp) { return cp < 128 ? 14 : 24; }
int bizud_draw(int x, int baseline, uint32_t cp, uint16_t fg, uint16_t bg)
{
    (void)baseline; (void)fg; (void)bg;
    if (cp == 'Z') last_z_x = x;
    return x + bizud_width(cp) <= 1280 ? bizud_width(cp) : 0;
}
int bizud_text(int x, int baseline, const char *text, int right, uint16_t fg, uint16_t bg)
{
    (void)baseline; (void)fg; (void)bg;
    assert(scheme_utf8_text(text, strlen(text)));
    while (*text) {
        int width = bizud_width(bizud_decode(&text));
        if (x + width > right) break;
        x += width;
    }
    return x;
}
bizime_point bizime_draw(const ime_view *view, int x, int baseline,
                         int left, int right, int top, int bottom, int pitch,
                         uint16_t fg, uint16_t bg, uint16_t accent)
{
    (void)view; (void)left; (void)right; (void)top; (void)bottom;
    (void)pitch; (void)fg; (void)bg; (void)accent;
    return (bizime_point){x, baseline};
}
bizime_point bizime_measure(const ime_view *view, int x, int baseline,
                            int left, int right, int pitch)
{
    (void)left;
    for (int n = 0; view && n < view->count; n++) {
        int advance = bizud_width(view->text[n]);
        if (x + advance > right) { x = left; baseline += pitch; }
        x += advance;
    }
    return (bizime_point){x, baseline};
}
int vfs_stat(const char *p, vfs_info_t *info)
{
    (void)p;
    if (!exists)
        return ERR_NOENT;
    info->type = FS_TYPE_FILE;
    info->size = (uint32_t)file_len;
    return 0;
}
int vfs_open(const char *p, uint32_t flags)
{
    (void)p;
    file_pos = 0;
    if (flags & VFS_O_TRUNC)
        file_len = 0;
    return 3;
}
int vfs_close(int fd)
{
    assert(fd == 3);
    return fail_close ? ERR_IO : 0;
}
int vfs_read(int fd, void *b, size_t n)
{
    assert(fd == 3);
    if (fail_read)
        return ERR_IO;
    if (n > 7)
        n = 7;
    if (n > file_len - file_pos)
        n = file_len - file_pos;
    memcpy(b, file_data + file_pos, n);
    file_pos += n;
    return (int)n;
}
int vfs_write(int fd, const void *b, size_t n)
{
    assert(fd == 3);
    if (fail_write)
        return ERR_IO;
    if (n > 5)
        n = 5;
    assert(file_pos + n < sizeof file_data);
    memcpy(file_data + file_pos, b, n);
    file_pos += n;
    file_len = file_pos;
    return (int)n;
}
int vfs_normalize(const char *p, char *out)
{
    if (p[0] != '/' || strlen(p) >= VFS_PATH_MAX)
        return ERR_INVAL;
    strcpy(out, p);
    return 0;
}

int main(void)
{
    ide *i = calloc(1, sizeof *i);
    assert(i);
    i->workspace = malloc(scheme_workspace_size());
    assert(i->workspace);
    reset(i);
    strcpy(i->path, "/home/test.scm");
    assert(scheme_buffer_set(&i->buffer, demo, sizeof demo - 1));
    script("");
    eval_source(i, false);
    assert(i->graph && i->points == 129);
    assert(!strcmp(i->plot_title, "減衰振動"));
    draw(i, NULL);
    assert(graph_lines > 100);
    /* Up/down follows proportional soft-wrapped screen rows, not only
     * newline-delimited logical lines. */
    char wrapped[121];
    memset(wrapped, 'm', sizeof wrapped - 1);
    wrapped[sizeof wrapped - 1] = 0;
    assert(scheme_buffer_set(&i->buffer, wrapped, sizeof wrapped - 1));
    source_layout(i, 735);
    assert(i->visual_rows >= 3);
    vertical(i, 1);
    size_t second_row = i->buffer.cursor;
    assert(second_row > 0 && second_row < i->buffer.length);
    vertical(i, -1);
    assert(i->buffer.cursor == 0);
    assert(scheme_buffer_set(&i->buffer, "ZZ", 2));
    ime_view composing = {.count = 1, .text = {0x65e5}};
    last_z_x = -1;
    draw(i, &composing);
    assert(last_z_x == UI_LEFT + 24 + 14); /* preedit pushes both suffix glyphs */
    assert(scheme_buffer_set(&i->buffer, demo, sizeof demo - 1));
    /* Scheme procedures can become M-x commands and modify the editor. */
    evaluate(i, "(define (挨拶) (insert-text! \"こんにちは\"))", false);
    script("挨\t\r");
    command(i);
    assert(!strncmp(i->buffer.text, "こんにちは", 15));
    scheme_buffer_undo(&i->buffer);
    assert(!strcmp(i->buffer.text, demo));
    char completion_text[128] = "eval-b", hint[256];
    complete_command(i, completion_text, sizeof completion_text, hint, sizeof hint);
    assert(!strcmp(completion_text, "eval-buffer"));
    strcpy(completion_text, "eval-");
    complete_command(i, completion_text, sizeof completion_text, hint, sizeof hint);
    assert(!strcmp(completion_text, "eval-") && strstr(hint, "eval-buffer") &&
           strstr(hint, "eval-expression"));
    strcpy(completion_text, "unknown");
    complete_command(i, completion_text, sizeof completion_text, hint, sizeof hint);
    assert(!strcmp(completion_text, "unknown") && !strcmp(hint, "No matching command"));
    evaluate(i, "(define (日本) 1) (define (日曜) 2) (define (引数あり x) x)", false);
    strcpy(completion_text, "日");
    complete_command(i, completion_text, sizeof completion_text, hint, sizeof hint);
    assert(!strcmp(completion_text, "日") &&
           scheme_utf8_text(completion_text, strlen(completion_text)));
    assert(strstr(hint, "日本") && strstr(hint, "日曜"));
    strcpy(completion_text, "引数");
    complete_command(i, completion_text, sizeof completion_text, hint, sizeof hint);
    assert(!strcmp(hint, "No matching command"));
    /* Last-expression evaluation skips previous side effects and works inside
     * an unfinished enclosing list, even with malformed text after point. */
    evaluate(i, "(define untouched 1)", false);
    const char *partial = "(set! untouched 9)\n(begin (+ 1 2) UNFINISHED";
    assert(scheme_buffer_set(&i->buffer, partial, strlen(partial)));
    i->buffer.cursor = (size_t)(strstr(partial, " UNFINISHED") - partial);
    script("");
    eval_source(i, true);
    assert(!strcmp(i->result, "3"));
    evaluate(i, "untouched", false);
    assert(!strcmp(i->result, "1"));
    assert(scheme_buffer_set(&i->buffer, demo, sizeof demo - 1));
    /* Escape-x survives decoding; arrows and cancellation remain separate. */
    script("\x1b"
           "x\x1b[D\x07");
    assert(key(i, 0) == META_X);
    assert(key(i, 0) == DQK_LEFT);
    assert(key(i, 0) == CTRL('g'));
    /* Minibuffer handles committed Japanese UTF-8 and removes whole characters. */
    char mini[32] = "";
    script("日本語\x7f\r");
    assert(minibuffer(i, "日本語", mini, sizeof mini));
    assert(!strcmp(mini, "日本"));
    script("abc\x07");
    assert(!minibuffer(i, "M-x", mini, sizeof mini));
    /* Evaluation cancellation preserves non-cancellation typeahead. */
    script("ab\x07"
           "c");
    assert(cancelled(i));
    assert(raw(i, 1) == 'a');
    assert(raw(i, 1) == 'b');
    assert(raw(i, 1) == 'c');
    script("");
    save(i);
    assert(!i->buffer.modified);
    assert(file_len == sizeof demo - 1 && !memcmp(file_data, demo, file_len));
    fail_write = true;
    i->buffer.modified = true;
    save(i);
    assert(i->buffer.modified && !strcmp(i->buffer.text, demo));
    fail_write = false;
    strcpy(file_data, "(display \"日本語\")");
    file_len = strlen("(display \"日本語\")");
    fail_read = true;
    assert(!load(i, "/home/a.scm", false));
    assert(!strcmp(i->buffer.text, demo));
    fail_read = false;
    fail_close = true;
    assert(!load(i, "/home/a.scm", false));
    assert(!strcmp(i->buffer.text, demo));
    fail_close = false;
    assert(load(i, "/home/a.scm", false));
    assert(!strcmp(i->buffer.text, "(display \"日本語\")"));
    file_data[0] = (char)0xc0;
    file_len = 1;
    assert(!load(i, "/home/b.scm", false));
    assert(!strcmp(i->path, "/home/a.scm"));
    memset(file_data, 'a', sizeof file_data);
    file_len = sizeof file_data;
    assert(!load(i, "/home/large.scm", false));
    double x[2] = {0, 1}, y[2] = {-1e100, 1e100};
    plot(i, x, y, 2, "大きな数");
    draw(i, NULL);
    i->buffer.cursor = i->buffer.length;
    draw(i, NULL);
    free(i->workspace);
    free(i);
    /* Startup failures report failure without leaving the console/display held. */
    exists = true;
    file_len = 1;
    file_data[0] = (char)0xc0;
    script("");
    assert(!scheme_ide("/home/invalid.scm"));
    assert(!claimed && !paused && allocations == 0);
    releases = 0;
    /* Full command entry/exit restores the console and releases allocations. */
    exists = false;
    script("\x18\x03");
    assert(scheme_ide("/home/new.scm"));
    assert(!claimed && !paused && allocations == 0 && releases == 1);
    script("\x18\x02\x11");
    assert(scheme_ide(NULL));
    assert(!claimed && !paused && allocations == 0 && releases == 2);
    puts("Scheme IDE: edit/eval/plot, Japanese minibuffer, M-x extension, VFS failures, cleanup "
         "PASS");
    return 0;
}

int vfs_replace_file(const char *path,const void *data,size_t len,size_t *written)
{
    return vfs_write_file(path,data,len,written);
}
