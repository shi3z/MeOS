#include <m5os/board_config.h>
#if M5OS_BOARD_TAB5
#include "../../drivers/lcd/fbcon.h"
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
#include <m5os/task.h>
#include <m5os/vfs.h>
#include <math.h>
#include <string.h>

#define CTRL(c) ((c) - 'a' + 1)
#define LOG_LINES 7
#define META_X 0x200
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
} ide;
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
static int key(ide *);
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
static size_t column(const scheme_buffer *b, size_t p)
{
    size_t n = 0;
    for (size_t k = start(b, p); k < p; k = scheme_buffer_next(b, k))
        n++;
    return n;
}
static void vertical(ide *i, int delta)
{
    scheme_buffer *b = &i->buffer;
    size_t col = column(b, b->cursor), p = start(b, b->cursor);
    if (delta < 0) {
        if (!p)
            return;
        p = start(b, p - 1);
    } else {
        p = end(b, p);
        if (p == b->length)
            return;
        p++;
    }
    while (col-- && p < b->length && b->text[p] != '\n')
        p = scheme_buffer_next(b, p);
    b->cursor = p;
}
static void draw_graph(ide *i, int left, int top, int width, int height)
{
    int cell = fbcon_cell_px(), x0 = (left + 1) * cell, y0 = (top + 2) * cell,
        w = (width - 2) * cell, h = (height - 4) * cell;
    dq_text(left + 1, top, i->graph ? i->plot_title : "*plot* / 関数グラフ");
    if (!i->graph || i->points < 2) {
        dq_text(left + 1, top + 3, "(plot wave 0 30)");
        return;
    }
    double lo = i->ys[0], hi = lo;
    for (unsigned p = 1; p < i->points; p++) {
        if (i->ys[p] < lo)
            lo = i->ys[p];
        if (i->ys[p] > hi)
            hi = i->ys[p];
    }
    /* Normalise before subtracting to avoid overflowing large finite ranges. */
    double scale = fmax(fabs(lo), fabs(hi));
    if (scale < 1)
        scale = 1;
    double low = lo / scale, range = hi / scale - low;
    if (range < 1e-12) {
        low -= .5;
        range = 1;
    }
    display_draw_rect(x0, y0, w, h, COLOR_GRAY);
    if (low <= 0 && low + range >= 0) {
        int y = y0 + h - 1 - (int)((-low) / range * (h - 1));
        display_draw_line(x0, y, x0 + w - 1, y, COLOR_GRAY);
    }
    int px = 0, py = 0;
    for (unsigned p = 0; p < i->points; p++) {
        int x = x0 + (int)(p * (unsigned)(w - 1) / (i->points - 1));
        double ratio = (i->ys[p] / scale - low) / range;
        if (ratio < 0)
            ratio = 0;
        if (ratio > 1)
            ratio = 1;
        int y = y0 + h - 1 - (int)(ratio * (h - 1));
        if (p)
            display_draw_line(px, py, x, y, COLOR_CYAN);
        px = x;
        py = y;
    }
    char a[32], b[32], line[100];
    number_format(i->xs[0], a, sizeof a);
    number_format(i->xs[i->points - 1], b, sizeof b);
    ksnprintf(line, sizeof line, "x: %s .. %s", a, b);
    dq_text(left + 1, top + height - 1, line);
}
static void draw(ide *i)
{
    scheme_buffer *b = &i->buffer;
    int cols = dq_cols(), rows = dq_rows();
    int width = i->split ? cols * 3 / 5 : cols;
    int textrows = rows - LOG_LINES - 5;
    size_t ls = start(b, b->cursor);
    if (ls < i->top)
        i->top = ls;
    int below = 0;
    for (size_t p = i->top; p < ls; p++)
        if (b->text[p] == '\n')
            below++;
    while (below >= textrows) {
        i->top = end(b, i->top) + 1;
        below--;
    }
    size_t col = column(b, b->cursor);
    if (col < i->horizontal)
        i->horizontal = col;
    if (col >= i->horizontal + (size_t)(width - 3))
        i->horizontal = col - (size_t)(width - 4);
    dq_clear();
    char line[256];
    ksnprintf(line, sizeof line, "MeOS Scheme  %s%s", b->modified ? "* " : "", i->path);
    dq_text_rev(0, 0, line);
    size_t p = i->top;
    for (int row = 0; row < textrows; row++) {
        size_t le = end(b, p), q = p;
        for (size_t skip = 0; skip < i->horizontal && q < le; skip++)
            q = scheme_buffer_next(b, q);
        size_t n = 0;
        int cells = 0;
        while (q < le && cells < width - 3) {
            size_t next = scheme_buffer_next(b, q), len = next - q;
            if (n + len >= sizeof line)
                break;
            if (b->text[q] == '\t')
                line[n++] = ' ';
            else {
                memcpy(line + n, b->text + q, len);
                n += len;
            }
            q = next;
            cells++;
        }
        line[n] = 0;
        dq_text(1, row + 2, line);
        if (p == ls)
            dq_cursor(0, row + 2, true);
        p = le < b->length ? le + 1 : b->length;
    }
    if (i->split)
        draw_graph(i, width, 2, cols - width, textrows);
    dq_text_rev(0, rows - LOG_LINES - 3, "*repl*  F2: expression | F1 / Esc x: commands");
    for (int row = 0; row < LOG_LINES; row++)
        dq_text(0, rows - LOG_LINES - 2 + row, i->log[row]);
    dq_text(0, rows - 2, i->status);
    ksnprintf(line, sizeof line, "%s | C-x C-s save  C-x C-b eval  C-x C-c quit", ime_status());
    dq_text_rev(0, rows - 1, line);
    int cx = 1 + (int)(col - i->horizontal), cy = 2 + below;
    /* Visible insertion marker; preedit/candidate overlays use this same cell. */
    display_fill_rect(cx * fbcon_cell_px(), (cy + 1) * fbcon_cell_px() - 2, fbcon_cell_px(), 2,
                      COLOR_CYAN);
    ime_set_anchor(cx, cy);
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
static int key(ide *i)
{
    int c = raw(i, 0);
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
static bool minibuffer(ide *i, const char *prompt, char *text, size_t cap)
{
    size_t len = strlen(text);
    bool commands = !strcmp(prompt, "M-x");
    char hint[256] = "Tab: complete command / C-g: cancel";
    for (;;) {
        int row = dq_rows() - 2;
        if (commands) {
            display_fill_rect(0, (row - 1) * fbcon_cell_px(), DISPLAY_WIDTH, fbcon_cell_px(),
                              COLOR_BLACK);
            dq_text(0, row - 1, hint);
        }
        display_fill_rect(0, row * fbcon_cell_px(), DISPLAY_WIDTH, fbcon_cell_px(), COLOR_BLACK);
        char line[320];
        ksnprintf(line, sizeof line, "%s: %s", prompt, text);
        /* Keep the end of the field visible, on codepoint boundaries. */
        const char *visible = line;
        size_t chars = 0;
        for (const char *p = line; *p; p++)
            if (((unsigned char)*p & 0xc0) != 0x80)
                chars++;
        while (chars > (size_t)dq_cols() - 1) {
            visible++;
            while (((unsigned char)*visible & 0xc0) == 0x80)
                visible++;
            chars--;
        }
        dq_text(0, row, visible);
        ime_set_anchor((int)chars, row);
        int k = key(i);
        if (k == DQK_ENTER) {
            ime_clear_anchor();
            return true;
        }
        if (k == DQK_ESC || k == CTRL('g')) {
            ime_clear_anchor();
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
    ide *i = kmalloc_psram(sizeof *i);
    if (!i) {
        kprintf("scheme: out of PSRAM\n");
        return false;
    }
    memset(i, 0, sizeof *i);
    i->workspace = kmalloc_psram(scheme_workspace_size());
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
        display_set_target(NULL);
        while (i->running) {
            draw(i);
            int k = key(i);
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
        ime_clear_anchor();
        display_set_target(NULL);
        fbcon_pause(false);
    }
    display_release_claim(i);
    kfree(i->workspace);
    kfree(i);
    return started;
}
#endif
