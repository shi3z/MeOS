#include <m5os/board_config.h>
#if M5OS_BOARD_TAB5
#include "../../drivers/lcd/fbcon.h"
#include "../../drivers/lcd/bizud.h"
#include "../editor/bizime.h"
#include "../tui/dqwin.h"
#include "buffer.h"
#include "ide.h"
#include "scheme.h"
#include <m5os/display.h>
#include <m5os/errno.h>
#include <m5os/ime.h>
#include <m5os/kmalloc.h>
#include <m5os/kprintf.h>
#include <m5os/number.h>
#include <m5os/sdroot.h>
#include <m5os/task.h>
#include <m5os/vfs.h>
#include <math.h>
#include <string.h>

#define CTRL(c) ((c) - 'a' + 1)
#define LOG_LINES 7
#define META_X 0x200
#define UI_BG RGB565(248,247,242)
#define UI_FG RGB565(36,39,42)
#define UI_ACCENT RGB565(25,85,115)
#define UI_COMMENT RGB565(55,110,75)
#define UI_STRING RGB565(145,70,55)
#define UI_LEFT 36
#define UI_RIGHT (DISPLAY_WIDTH-36)
#define UI_HEADER_BASE 36
#define UI_BODY_BASE 82
#define UI_PITCH 34
#define UI_BODY_ROWS 12
#define UI_REPL_TOP 500
#define UI_REPL_LINES 3
static const char *const command_names[] = {
    "eval-buffer", "eval-last-sexp",    "eval-expression",      "save-buffer",        "find-file",
    "undo",        "reset-environment", "delete-other-windows", "split-window-right", "quit",
    NULL};
typedef struct {
    scheme_buffer buffer;
    scheme *vm;
    void *workspace;
    char path[VFS_PATH_MAX], status[160], result[512];
    char plot_title[128];
    char log[LOG_LINES][160];
    unsigned log_line, log_len;
    size_t top, horizontal;
    bool graph, split, prefix, running;
    unsigned points;
    unsigned pending_head, pending_count;
    unsigned char pending[256];
    double xs[129], ys[129];
    char staging[SCHEME_SOURCE_CAP + 1];
    char commit[IME_COMMIT_BYTES];
    size_t starts[SCHEME_SOURCE_CAP + 2];
    int visual_rows;
    uint16_t *frame;
} ide;
static void *editor_alloc(size_t bytes)
{
    void *p = kmalloc_psram(bytes);
    if (!p && bizud_trim()) p = kmalloc_psram(bytes);
    if (!p && sdroot_asset_cache_trim()) p = kmalloc_psram(bytes);
    return p;
}
static const char demo[] = "; Scheme workspace / 日本語入力: Ctrl-Space\n"
                           "; C-x C-b: evaluate buffer    F2: REPL\n"
                           "(define (wave x)\n"
                           "  (* (sin x) (exp (* -0.1 x))))\n"
                           "\n"
                           "(display \"減衰振動\")\n"
                           "(newline)\n"
                           "(plot wave 0 30 \"減衰振動\")\n";
static void message(ide *i, const char *s) { ksnprintf(i->status, sizeof i->status, "%s", s); }
static void output(void *ctx, const char *s)
{
    ide *i = ctx;
    while (*s) {
        unsigned n = (unsigned char)*s >= 0xf0   ? 4
                     : (unsigned char)*s >= 0xe0 ? 3
                     : (unsigned char)*s >= 0xc0 ? 2
                                                 : 1;
        if (*s == '\n' || i->log_len + n >= sizeof i->log[0]) {
            if (i->log_line == LOG_LINES - 1) {
                memmove(i->log, i->log + 1, (LOG_LINES - 1) * sizeof i->log[0]);
            } else
                i->log_line++;
            i->log[i->log_line][0] = 0;
            i->log_len = 0;
            if (*s == '\n') {
                s++;
                continue;
            }
        }
        /* Interpreter output may end mid-codepoint after bounded printing. */
        unsigned available = 0;
        while (available < n && s[available])
            available++;
        if (available != n)
            break;
        memcpy(i->log[i->log_line] + i->log_len, s, n);
        i->log_len += n;
        i->log[i->log_line][i->log_len] = 0;
        s += n;
    }
}
static void plot(void *ctx, const double *x, const double *y, unsigned n, const char *title)
{
    ide *i = ctx;
    if (n > 129)
        return;
    memcpy(i->xs, x, n * sizeof *x);
    memcpy(i->ys, y, n * sizeof *y);
    i->points = n;
    i->graph = true;
    i->split = true;
    ksnprintf(i->plot_title, sizeof i->plot_title, "%s", title);
}
static bool cancelled(void *ctx)
{
    ide *i = ctx;
    task_sleep(0);
    for (unsigned n = 0; n < 32; n++) {
        /* Stop without consuming another byte if the typeahead queue is full. */
        if (i->pending_count == sizeof i->pending)
            return true;
        int c = kconsole_getc();
        if (c < 0)
            break;
        if (c == CTRL('g') || c == CTRL('c'))
            return true;
        i->pending[(i->pending_head + i->pending_count) % sizeof i->pending] = (unsigned char)c;
        i->pending_count++;
    }
    return false;
}
static int raw(ide *, unsigned);
static int key(ide *, unsigned);
static bool minibuffer(ide *, const char *, char *, size_t);
static bool insert_text(void *ctx, const char *text)
{
    ide *i = ctx;
    return scheme_buffer_insert(&i->buffer, text, strlen(text));
}
static size_t start(const scheme_buffer *b, size_t p)
{
    while (p && b->text[p - 1] != '\n')
        p--;
    return p;
}
static size_t end(const scheme_buffer *b, size_t p)
{
    while (p < b->length && b->text[p] != '\n')
        p++;
    return p;
}
static uint32_t source_cp(const scheme_buffer *b, size_t p)
{
    const char *text = b->text + p;
    return bizud_decode(&text);
}

static int source_advance(const scheme_buffer *b, size_t p)
{
    uint32_t cp = source_cp(b, p);
    return cp == '\t' ? 48 : cp == '\r' ? 0 : bizud_width(cp);
}

static void source_layout(ide *i, int right)
{
    scheme_buffer *b = &i->buffer;
    int x = UI_LEFT;
    i->visual_rows = 1;
    i->starts[0] = 0;
    for (size_t p = 0; p < b->length;) {
        uint32_t cp = source_cp(b, p);
        int advance = source_advance(b, p);
        if (cp != '\n' && x + advance > right) {
            i->starts[i->visual_rows++] = p;
            x = UI_LEFT;
        }
        p = scheme_buffer_next(b, p);
        if (cp == '\n') {
            i->starts[i->visual_rows++] = p;
            x = UI_LEFT;
        } else
            x += advance;
    }
    i->starts[i->visual_rows] = b->length;
}

static int source_cursor_row(ide *i)
{
    int row = 0;
    while (row + 1 < i->visual_rows && i->starts[row + 1] <= i->buffer.cursor) row++;
    return row;
}

static int source_cursor_x(ide *i, int row)
{
    int x = UI_LEFT;
    for (size_t p = i->starts[row]; p < i->buffer.cursor; p = scheme_buffer_next(&i->buffer, p))
        x += source_advance(&i->buffer, p);
    return x;
}

static void vertical(ide *i, int delta)
{
    scheme_buffer *b = &i->buffer;
    source_layout(i, i->split ? 735 : UI_RIGHT);
    int row = source_cursor_row(i), target = row + delta;
    if (target < 0) target = 0;
    if (target >= i->visual_rows) target = i->visual_rows - 1;
    int wanted = source_cursor_x(i, row), x = UI_LEFT;
    size_t p = i->starts[target];
    while (p < i->starts[target + 1] && source_cp(b, p) != '\n') {
        int advance = source_advance(b, p);
        if (x + advance / 2 >= wanted) break;
        x += advance;
        p = scheme_buffer_next(b, p);
    }
    if (target + 1 < i->visual_rows && p == i->starts[target + 1] && p > i->starts[target])
        p = scheme_buffer_prev(b, p);
    b->cursor = p;
}

static void syntax_state(const scheme_buffer *b, size_t at, bool *string, bool *escape, bool *comment)
{
    *string = *escape = *comment = false;
    for (size_t p = start(b, at); p < at; p++) {
        char c = b->text[p];
        if (*comment) continue;
        if (*escape) { *escape = false; continue; }
        if (*string && c == '\\') { *escape = true; continue; }
        if (c == '"') *string = !*string;
        else if (!*string && c == ';') *comment = true;
    }
}

static void draw_source_row(ide *i, int row, int baseline, int right)
{
    scheme_buffer *b = &i->buffer;
    int x = UI_LEFT;
    bool string, escape, comment;
    syntax_state(b, i->starts[row], &string, &escape, &comment);
    for (size_t p = i->starts[row]; p < i->starts[row + 1]; p = scheme_buffer_next(b, p)) {
        uint32_t cp = source_cp(b, p);
        if (cp == '\n') break;
        int advance = source_advance(b, p);
        uint16_t color = comment ? UI_COMMENT : string ? UI_STRING : UI_FG;
        if (!comment && !string && (cp == '(' || cp == ')' || cp == '\'')) color = UI_ACCENT;
        if (cp == '\t') {
            x += advance;
            continue;
        }
        if (x + advance > right) break;
        bizud_draw(x, baseline, cp, color, UI_BG);
        x += advance;
        if (!comment) {
            if (escape) escape = false;
            else if (string && cp == '\\') escape = true;
            else if (cp == '"') string = !string;
            else if (!string && cp == ';') comment = true;
        }
    }
}

static void draw_reflowed_tail(ide *i, bizime_point point, int right)
{
    scheme_buffer *b = &i->buffer;
    int x = point.x, baseline = point.baseline;
    bool string, escape, comment;
    syntax_state(b, b->cursor, &string, &escape, &comment);
    for (size_t p = b->cursor; p < b->length; p = scheme_buffer_next(b, p)) {
        uint32_t cp = source_cp(b, p);
        if (cp == '\n') {
            x = UI_LEFT;
            baseline += UI_PITCH;
            string = escape = comment = false;
            continue;
        }
        int advance = source_advance(b, p);
        if (x + advance > right) {
            x = UI_LEFT;
            baseline += UI_PITCH;
        }
        if (baseline >= UI_REPL_TOP - 8) break;
        uint16_t color = comment ? UI_COMMENT : string ? UI_STRING : UI_FG;
        if (!comment && !string && (cp == '(' || cp == ')' || cp == '\'')) color = UI_ACCENT;
        if (cp != '\t' && cp != '\r') bizud_draw(x, baseline, cp, color, UI_BG);
        x += advance;
        if (!comment) {
            if (escape) escape = false;
            else if (string && cp == '\\') escape = true;
            else if (cp == '"') string = !string;
            else if (!string && cp == ';') comment = true;
        }
    }
}

static void draw_graph(ide *i, int left, int top, int width, int height)
{
    bizud_text(left + 14, top + 28, i->graph ? i->plot_title : "*plot* / 関数グラフ",
               left + width - 14, UI_ACCENT, UI_BG);
    int x0 = left + 14, y0 = top + 46, w = width - 28, h = height - 82;
    if (!i->graph || i->points < 2) {
        bizud_text(x0, y0 + 34, "(plot wave 0 30)", left + width - 14, UI_FG, UI_BG);
        return;
    }
    double lo = i->ys[0], hi = lo;
    for (unsigned p = 1; p < i->points; p++) {
        if (i->ys[p] < lo) lo = i->ys[p];
        if (i->ys[p] > hi) hi = i->ys[p];
    }
    double scale = fmax(fabs(lo), fabs(hi));
    if (scale < 1) scale = 1;
    double low = lo / scale, range = hi / scale - low;
    if (range < 1e-12) { low -= .5; range = 1; }
    display_draw_rect(x0, y0, w, h, UI_ACCENT);
    if (low <= 0 && low + range >= 0) {
        int y = y0 + h - 1 - (int)((-low) / range * (h - 1));
        display_draw_line(x0, y, x0 + w - 1, y, COLOR_GRAY);
    }
    int px = 0, py = 0;
    for (unsigned p = 0; p < i->points; p++) {
        int x = x0 + (int)(p * (unsigned)(w - 1) / (i->points - 1));
        double ratio = (i->ys[p] / scale - low) / range;
        if (ratio < 0) ratio = 0;
        if (ratio > 1) ratio = 1;
        int y = y0 + h - 1 - (int)(ratio * (h - 1));
        if (p) display_draw_line(px, py, x, y, UI_ACCENT);
        px = x; py = y;
    }
    char a[32], b[32], line[100];
    number_format(i->xs[0], a, sizeof a);
    number_format(i->xs[i->points - 1], b, sizeof b);
    ksnprintf(line, sizeof line, "x: %s .. %s", a, b);
    bizud_text(x0, top + height - 8, line, left + width - 14, UI_FG, UI_BG);
}

static void draw(ide *i, const ime_view *view)
{
    scheme_buffer *b = &i->buffer;
    int editor_right = i->split ? 735 : UI_RIGHT;
    source_layout(i, editor_right);
    int cursor_row = source_cursor_row(i);
    if ((int)i->top > cursor_row) i->top = (size_t)cursor_row;
    if (cursor_row >= (int)i->top + UI_BODY_ROWS)
        i->top = (size_t)(cursor_row - UI_BODY_ROWS + 1);

    display_set_target(i->frame);
    display_fill_rect(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT, UI_BG);
    display_fill_rect(0, 0, DISPLAY_WIDTH, 52, UI_ACCENT);
    char line[256];
    ksnprintf(line, sizeof line, "MeOS Scheme  %s%s", b->modified ? "* " : "", i->path);
    bizud_text(UI_LEFT, UI_HEADER_BASE, line, UI_RIGHT, COLOR_WHITE, UI_ACCENT);

    for (int n = 0; n < UI_BODY_ROWS; n++) {
        int row = (int)i->top + n;
        if (row >= i->visual_rows) break;
        draw_source_row(i, row, UI_BODY_BASE + n * UI_PITCH, editor_right);
    }
    int visible_row = cursor_row - (int)i->top;
    if (visible_row >= 0 && visible_row < UI_BODY_ROWS) {
        int cx = source_cursor_x(i, cursor_row);
        int cy = UI_BODY_BASE + visible_row * UI_PITCH;
        if (view && view->count) {
            int next = cy + UI_PITCH - 27;
            display_fill_rect(cx, cy - 27, editor_right - cx, 32, UI_BG);
            if (next < UI_REPL_TOP - 8)
                display_fill_rect(UI_LEFT, next, editor_right - UI_LEFT,
                                  UI_REPL_TOP - 8 - next, UI_BG);
            bizime_point after = bizime_measure(view, cx, cy, UI_LEFT, editor_right, UI_PITCH);
            draw_reflowed_tail(i, after, editor_right);
            bizime_draw(view, cx, cy, UI_LEFT, editor_right, 53, UI_REPL_TOP - 8,
                        UI_PITCH, UI_FG, UI_BG, UI_ACCENT);
        } else
            display_fill_rect(cx, cy - 27, 2, 32, UI_ACCENT);
    }
    if (i->split) {
        display_fill_rect(751, 58, 1, UI_REPL_TOP - 66, UI_ACCENT);
        draw_graph(i, 752, 58, UI_RIGHT - 752, UI_REPL_TOP - 66);
    }

    display_fill_rect(UI_LEFT, UI_REPL_TOP, UI_RIGHT - UI_LEFT, 1, UI_ACCENT);
    bizud_text(UI_LEFT, UI_REPL_TOP + 31, "*repl*  F2: expression / F1: M-x",
               UI_RIGHT, UI_ACCENT, UI_BG);
    int first = (int)i->log_line - UI_REPL_LINES + 1;
    if (first < 0) first = 0;
    for (int n = 0; n < UI_REPL_LINES && first + n <= (int)i->log_line; n++)
        bizud_text(UI_LEFT, UI_REPL_TOP + 65 + n * 32, i->log[first + n],
                   UI_RIGHT, UI_FG, UI_BG);
    bizud_text(UI_LEFT, 668, i->status, UI_RIGHT, UI_ACCENT, UI_BG);
    display_fill_rect(0, 681, DISPLAY_WIDTH, 39, UI_ACCENT);
    ksnprintf(line, sizeof line, "%s | C-x C-s save  C-x C-b eval  C-x C-c quit", ime_status());
    bizud_text(UI_LEFT, 710, line, UI_RIGHT, COLOR_WHITE, UI_ACCENT);
    display_present(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    display_set_target(NULL);
}
static bool load(ide *i, const char *path, bool allow_missing)
{
    vfs_info_t info;
    int rc = vfs_stat(path, &info);
    if (rc == ERR_NOENT && allow_missing) {
        scheme_buffer_set(&i->buffer, "", 0);
        strcpy(i->path, path);
        return true;
    }
    if (rc < 0 || info.type != FS_TYPE_FILE) {
        message(i, "Cannot open a regular source file");
        return false;
    }
    int fd = vfs_open(path, VFS_O_READ);
    if (fd < 0) {
        message(i, "Open failed");
        return false;
    }
    size_t n = 0;
    bool ok = true;
    while (n < sizeof i->staging) {
        rc = vfs_read(fd, i->staging + n, sizeof i->staging - n);
        if (rc < 0) {
            ok = false;
            break;
        }
        if (!rc)
            break;
        n += (size_t)rc;
    }
    if (vfs_close(fd) < 0)
        ok = false;
    if (!ok || !scheme_buffer_set(&i->buffer, i->staging, n)) {
        message(i, "Load failed: I/O, size limit, or invalid UTF-8");
        return false;
    }
    strcpy(i->path, path);
    i->top = i->horizontal = 0;
    message(i, "Loaded");
    return true;
}
static void save(ide *i)
{
    bool ok = vfs_replace_file(i->path, i->buffer.text, i->buffer.length, NULL) == ERR_OK;
    if (ok) {
        i->buffer.modified = false;
        message(i, "Saved to VFS (persistence depends on mounted storage)");
    } else
        message(i, "Save failed: file may be partial; source retained");
}
static void evaluate(ide *i, const char *source, bool last)
{
    ime_clear_anchor();
    bool ok = scheme_eval(i->vm, source, last, i->result, sizeof i->result);
    output(i, ok ? "=> " : "error: ");
    output(i, i->result);
    output(i, "\n");
    if (ok)
        message(i, "Evaluated");
    else
        ksnprintf(i->status, sizeof i->status, "Scheme error at byte %u: %s",
                  (unsigned)scheme_error_offset(i->vm), i->result);
}
static void eval_source(ide *i, bool last)
{
    size_t begin = 0, end = i->buffer.length;
    if (last && !scheme_buffer_previous_form(&i->buffer, i->buffer.cursor, &begin, &end)) {
        message(i, "No complete expression before point");
        return;
    }
    size_t n = end - begin;
    memcpy(i->staging, i->buffer.text + begin, n);
    i->staging[n] = 0;
    evaluate(i, i->staging, false);
}
static void repl(ide *i)
{
    char expression[256] = "";
    ime_clear_anchor();
    if (minibuffer(i, "Scheme expression", expression, sizeof expression)) {
        output(i, "> ");
        output(i, expression);
        output(i, "\n");
        evaluate(i, expression, false);
    }
}
static void reset(ide *i)
{
    i->vm = scheme_init(i->workspace, (scheme_host){.context = i,
                                                    .write = output,
                                                    .plot = plot,
                                                    .cancel = cancelled,
                                                    .insert_text = insert_text});
    message(i, "Environment reset; source retained");
}
static void quit(ide *i)
{
    ime_clear_anchor();
    if (!i->buffer.modified || dq_confirm("Discard unsaved changes?"))
        i->running = false;
}
static void command(ide *i)
{
    char name[96] = "";
    ime_clear_anchor();
    if (!minibuffer(i, "M-x", name, sizeof name))
        return;
    if (!strcmp(name, "eval-buffer"))
        eval_source(i, false);
    else if (!strcmp(name, "eval-last-sexp"))
        eval_source(i, true);
    else if (!strcmp(name, "eval-expression"))
        repl(i);
    else if (!strcmp(name, "save-buffer"))
        save(i);
    else if (!strcmp(name, "undo"))
        scheme_buffer_undo(&i->buffer);
    else if (!strcmp(name, "reset-environment"))
        reset(i);
    else if (!strcmp(name, "delete-other-windows"))
        i->split = false;
    else if (!strcmp(name, "split-window-right"))
        i->split = true;
    else if (!strcmp(name, "quit"))
        quit(i);
    else if (!strcmp(name, "find-file")) {
        char path[VFS_PATH_MAX] = "";
        if (i->buffer.modified && !dq_confirm("Discard unsaved changes?"))
            return;
        if (minibuffer(i, "Absolute source path", path, sizeof path)) {
            char normalized[VFS_PATH_MAX];
            if (path[0] != '/' || vfs_normalize(path, normalized) < 0)
                message(i, "Use an absolute VFS path");
            else
                load(i, normalized, true);
        }
    } else {
        bool valid = name[0] != 0;
        for (const char *p = name; *p; p++)
            if ((unsigned char)*p <= 32 || strchr("()\";'", *p))
                valid = false;
        if (valid) {
            char call[100];
            ksnprintf(call, sizeof call, "(%s)", name);
            evaluate(i, call, false);
        } else
            message(i, "Command must be a Scheme procedure name");
    }
}
/* Own decoder preserves ESC x, which dq_getkey intentionally swallows. IME
 * still runs upstream of this decoder, including Ctrl-Space and candidates. */
static int raw(ide *i, unsigned wait)
{
    if (i->pending_count) {
        int c = i->pending[i->pending_head];
        i->pending_head = (i->pending_head + 1) % sizeof i->pending;
        i->pending_count--;
        return c;
    }
    for (unsigned t = 0;; t += 10) {
        int c = kconsole_getc();
        if (c >= 0)
            return c;
        if (wait && t >= wait)
            return -1;
        task_sleep(10);
    }
}
static int key(ide *i, unsigned wait)
{
    int c = raw(i, wait);
    if (c < 0)
        return DQK_NONE;
    if (c == '\n')
        return DQK_ENTER;
    if (c == 8)
        return DQK_BACKSPACE;
    if (c != 27)
        return c;
    int a = raw(i, 40);
    if (a == 'x')
        return META_X;
    if (a != '[' && a != 'O')
        return DQK_ESC;
    char seq[12];
    unsigned n = 0;
    int b;
    while (n < sizeof seq - 1) {
        b = raw(i, 40);
        if (b < 0)
            break;
        seq[n++] = (char)b;
        if (b >= 0x40 && b <= 0x7e)
            break;
    }
    seq[n] = 0;
    if (!strcmp(seq, "A"))
        return DQK_UP;
    if (!strcmp(seq, "B"))
        return DQK_DOWN;
    if (!strcmp(seq, "C"))
        return DQK_RIGHT;
    if (!strcmp(seq, "D"))
        return DQK_LEFT;
    if (!strcmp(seq, "H") || !strcmp(seq, "1~"))
        return DQK_HOME;
    if (!strcmp(seq, "F") || !strcmp(seq, "4~"))
        return DQK_END;
    if (!strcmp(seq, "3~"))
        return DQK_DEL;
    if (!strcmp(seq, "P") || !strcmp(seq, "11~"))
        return DQK_F1;
    if (!strcmp(seq, "Q") || !strcmp(seq, "12~"))
        return DQK_F2;
    if (!strcmp(seq, "R") || !strcmp(seq, "13~"))
        return DQK_F3;
    return DQK_NONE;
}

typedef struct {
    const char *prefix;
    unsigned count;
    char common[128], names[256];
} completion;
static void offer_completion(void *context, const char *name)
{
    completion *c = context;
    if (strncmp(name, c->prefix, strlen(c->prefix)))
        return;
    if (!c->count)
        strcpy(c->common, name);
    else {
        size_t n = 0;
        while (c->common[n] && name[n] && c->common[n] == name[n])
            n++;
        /* Common byte prefixes can end inside a Japanese character. */
        while (n && ((unsigned char)c->common[n] & 0xc0) == 0x80)
            n--;
        c->common[n] = 0;
    }
    size_t used = strlen(c->names), len = strlen(name);
    if (used + len + (used ? 2 : 0) < sizeof c->names) {
        if (used)
            strcat(c->names, "  ");
        strcat(c->names, name);
    }
    c->count++;
}
static void offer_user_command(void *context, const char *name)
{
    for (unsigned n = 0; command_names[n]; n++)
        if (!strcmp(name, command_names[n]))
            return; /* built-in dispatch takes precedence */
    offer_completion(context, name);
}
static void complete_command(ide *i, char *text, size_t capacity, char *hint, size_t hint_capacity)
{
    completion c = {.prefix = text};
    for (unsigned n = 0; command_names[n]; n++)
        offer_completion(&c, command_names[n]);
    scheme_visit_commands(i->vm, offer_user_command, &c);
    if (c.count && strlen(c.common) < capacity)
        strcpy(text, c.common);
    ksnprintf(hint, hint_capacity, "%s", c.count ? c.names : "No matching command");
}

static int text_pixels(const char *text)
{
    int width = 0;
    while (*text) width += bizud_width(bizud_decode(&text));
    return width;
}

static const char *visible_tail(const char *text, int width)
{
    while (*text && text_pixels(text) > width) {
        text++;
        while (((unsigned char)*text & 0xc0) == 0x80) text++;
    }
    return text;
}

static void draw_minibuffer(ide *i, const char *prompt, const char *text,
                            const char *hint, bool commands, const ime_view *view)
{
    draw(i, NULL);
    display_set_target(i->frame);
    int top = commands ? 548 : 580;
    display_fill_rect(0, top, DISPLAY_WIDTH, DISPLAY_HEIGHT - top, UI_BG);
    display_fill_rect(0, top, DISPLAY_WIDTH, 1, UI_ACCENT);
    if (commands)
        bizud_text(UI_LEFT, top + 31, hint, UI_RIGHT, UI_COMMENT, UI_BG);
    char prefix[128];
    ksnprintf(prefix, sizeof prefix, "%s: ", prompt);
    int prefix_width = text_pixels(prefix);
    int baseline = commands ? 624 : 626;
    bizud_text(UI_LEFT, baseline, prefix, UI_RIGHT, UI_ACCENT, UI_BG);
    const char *visible = visible_tail(text, UI_RIGHT - UI_LEFT - prefix_width);
    int x = bizud_text(UI_LEFT + prefix_width, baseline, visible, UI_RIGHT, UI_FG, UI_BG);
    if (view && view->count)
        bizime_draw(view, x, baseline, UI_LEFT, UI_RIGHT, 54, 679,
                    UI_PITCH, UI_FG, UI_BG, UI_ACCENT);
    else
        display_fill_rect(x, baseline - 27, 2, 32, UI_ACCENT);
    char mode[96];
    ksnprintf(mode, sizeof mode, "%s | Enter: accept  C-g: cancel", ime_status());
    display_fill_rect(0, 681, DISPLAY_WIDTH, 39, UI_ACCENT);
    bizud_text(UI_LEFT, 710, mode, UI_RIGHT, COLOR_WHITE, UI_ACCENT);
    display_present(0, top, DISPLAY_WIDTH, DISPLAY_HEIGHT - top);
    display_set_target(NULL);
}

static bool minibuffer(ide *i, const char *prompt, char *text, size_t cap)
{
    size_t len = strlen(text);
    bool commands = !strcmp(prompt, "M-x");
    char hint[256] = "Tab: complete command / C-g: cancel";
    uint32_t revision = ~0u;
    bool redraw = true;
    for (;;) {
        size_t committed = ime_take_commit(i->commit, sizeof i->commit);
        if (committed) {
            if (len + committed < cap && scheme_utf8_text(i->commit, committed)) {
                memcpy(text + len, i->commit, committed);
                len += committed; text[len] = 0;
            } else {
                ksnprintf(hint, sizeof hint, "Input rejected: invalid UTF-8 or full buffer");
            }
            redraw = true;
        }
        ime_view view;
        ime_read_view(&view);
        if (redraw || revision != view.revision) {
            draw_minibuffer(i, prompt, text, hint, commands, &view);
            revision = view.revision;
            redraw = false;
        }
        int k = key(i, 10);
        if (k == DQK_NONE) continue;
        redraw = true;
        if (k == DQK_ENTER) {
            return true;
        }
        if (k == DQK_ESC || k == CTRL('g')) {
            return false;
        }
        if (k == '\t' && commands) {
            complete_command(i, text, cap, hint, sizeof hint);
            len = strlen(text);
        } else if (k == DQK_BACKSPACE || k == DQK_DEL) {
            if (len) {
                len--;
                while (len && ((unsigned char)text[len] & 0xc0) == 0x80)
                    len--;
                text[len] = 0;
            }
        } else if (k == CTRL('u')) {
            len = 0;
            text[0] = 0;
        } else if (k >= 32 && k < 256 && k != 127) {
            char bytes[4];
            unsigned n = k >= 0xf0 ? 4 : k >= 0xe0 ? 3 : k >= 0xc2 ? 2 : 1;
            bytes[0] = (char)k;
            bool ok = true;
            for (unsigned p = 1; p < n; p++) {
                int c = raw(i, 100);
                if (c < 0) {
                    ok = false;
                    break;
                }
                bytes[p] = (char)c;
            }
            if (ok && len + n < cap && scheme_utf8_text(bytes, n)) {
                memcpy(text + len, bytes, n);
                len += n;
                text[len] = 0;
            }
        }
    }
}
bool scheme_ide(const char *path)
{
    if (!display_is_ready() || fbcon_is_paused()) {
        kprintf("scheme: display unavailable or busy\n");
        return false;
    }
    ide *i = editor_alloc(sizeof *i);
    if (!i) {
        kprintf("scheme: out of PSRAM\n");
        return false;
    }
    memset(i, 0, sizeof *i);
    i->workspace = editor_alloc(scheme_workspace_size());
    if (!i->workspace || !display_try_claim(i)) {
        kprintf("scheme: memory or display unavailable\n");
        kfree(i->workspace);
        kfree(i);
        return false;
    }
    bool started = true;
    reset(i);
    i->running = i->split = true;
    if (path) {
        if (!load(i, path, true)) {
            kprintf("scheme: %s\n", i->status);
            i->running = false;
            started = false;
        }
    } else {
        strcpy(i->path, "/home/workspace.scm");
        scheme_buffer_set(&i->buffer, demo, sizeof demo - 1);
        i->buffer.modified = true;
        message(i, "C-x C-b evaluates the demo. F3: help. Ctrl-Space: 日本語");
    }
    if (i->running) {
        fbcon_pause(true);
        task_sleep(100);
        i->frame = editor_alloc(DISPLAY_BUFFER_BYTES);
        if (!i->frame || !bizud_begin()) {
            kprintf("scheme: BIZ UD display memory unavailable\n");
            kfree(i->frame);
            i->frame = NULL;
            i->running = false;
            started = false;
        }
        ime_set_app_view(true);
        uint32_t revision = ~0u;
        bool redraw = true;
        while (i->running) {
            size_t committed = ime_take_commit(i->commit, sizeof i->commit);
            if (committed) {
                if (!scheme_buffer_insert(&i->buffer, i->commit, committed))
                    message(i, "Insertion rejected: invalid UTF-8 or full buffer");
                redraw = true;
            }
            ime_view view;
            ime_read_view(&view);
            if (redraw || revision != view.revision) {
                draw(i, &view);
                revision = view.revision;
                redraw = false;
            }
            int k = key(i, 10);
            if (k == DQK_NONE) continue;
            redraw = true;
            scheme_buffer *b = &i->buffer;
            if (i->prefix) {
                i->prefix = false;
                if (k == CTRL('s'))
                    save(i);
                else if (k == CTRL('b'))
                    eval_source(i, false);
                else if (k == CTRL('e'))
                    eval_source(i, true);
                else if (k == CTRL('c'))
                    quit(i);
                else if (k == '1')
                    i->split = false;
                else if (k == '2')
                    i->split = true;
                else if (k == 'x')
                    command(i);
                else
                    message(i, "Unknown C-x command");
                continue;
            }
            switch (k) {
            case CTRL('x'):
                i->prefix = true;
                message(i, "C-x ...");
                break;
            case META_X:
            case DQK_F1:
                command(i);
                break;
            case DQK_F2:
                repl(i);
                break;
            case DQK_F3:
                ime_clear_anchor();
                dq_message("M-x: eval-buffer eval-last-sexp eval-expression",
                           "save-buffer find-file undo reset-environment quit");
                break;
            case CTRL('g'):
            case DQK_ESC:
                message(i, "Cancelled");
                break;
            case CTRL('s'):
                save(i);
                break;
            case CTRL('q'):
                quit(i);
                break;
            case 0x1f:
                scheme_buffer_undo(b);
                break;
            case CTRL('a'):
            case DQK_HOME:
                b->cursor = start(b, b->cursor);
                break;
            case CTRL('e'):
            case DQK_END:
                b->cursor = end(b, b->cursor);
                break;
            case CTRL('b'):
            case DQK_LEFT:
                b->cursor = scheme_buffer_prev(b, b->cursor);
                break;
            case CTRL('f'):
            case DQK_RIGHT:
                b->cursor = scheme_buffer_next(b, b->cursor);
                break;
            case CTRL('p'):
            case DQK_UP:
                vertical(i, -1);
                break;
            case CTRL('n'):
            case DQK_DOWN:
                vertical(i, 1);
                break;
            case DQK_BACKSPACE:
                scheme_buffer_delete(b, true);
                break;
            case CTRL('d'):
            case DQK_DEL:
                scheme_buffer_delete(b, false);
                break;
            case DQK_ENTER:
                scheme_buffer_insert(b, "\n", 1);
                break;
            case '\t':
                scheme_buffer_insert(b, "  ", 2);
                break;
            default:
                if (k >= 32 && k < 256 && k != 127) {
                    char text[4];
                    unsigned n = k >= 0xf0 ? 4 : k >= 0xe0 ? 3 : k >= 0xc2 ? 2 : 1;
                    text[0] = (char)k;
                    bool ok = true;
                    for (unsigned p = 1; p < n; p++) {
                        int v = raw(i, 100);
                        if (v < 0) {
                            ok = false;
                            break;
                        }
                        text[p] = (char)v;
                    }
                    if (!ok || !scheme_buffer_insert(b, text, n))
                        message(i, "Insertion rejected: invalid UTF-8 or full buffer");
                }
                break;
            }
        }
        ime_set_app_view(false);
        ime_clear_anchor();
        display_set_target(NULL);
        fbcon_pause(false);
        if (i->frame) {
            bizud_end();
            kfree(i->frame);
        }
    }
    display_release_claim(i);
    kfree(i->workspace);
    kfree(i);
    return started;
}
#endif
