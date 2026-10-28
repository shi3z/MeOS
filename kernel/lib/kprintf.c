#include <m5os/kprintf.h>
#include <m5os/hal_console.h>
#include <m5os/mutex.h>
#include <m5os/task.h>
#include <m5os/arch.h>
#include <stdarg.h>

#define NUM_BUF   32
#define RADIX_DEC 10u
#define RADIX_HEX 16u

typedef void (*out_fn)(char c, void *ctx);

/* ---- formatting core: writes characters through a callback ----------------- */

static void pad_out(out_fn out, void *ctx, const char *s, int len, int width, char pad, bool left)
{
    if (!left) for (int i = len; i < width; i++) out(pad, ctx);
    for (int i = 0; i < len; i++) out(s[i], ctx);
    if (left) for (int i = len; i < width; i++) out(' ', ctx);
}

static int utoa_radix(uint32_t v, unsigned radix, char *rev)
{
    static const char digits[] = "0123456789abcdef";
    int n = 0;
    do { rev[n++] = digits[v % radix]; v /= radix; } while (v);
    return n;
}

static void vformat(out_fn out, void *ctx, const char *fmt, va_list ap)
{
    for (; *fmt; fmt++) {
        if (*fmt != '%') { out(*fmt, ctx); continue; }
        fmt++;

        bool left = false;
        char pad = ' ';
        int width = 0;
        if (*fmt == '-') { left = true; fmt++; }
        if (*fmt == '0') { pad = '0'; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');

        char tmp[NUM_BUF], rev[NUM_BUF];
        int k = 0, n;
        switch (*fmt) {
        case 'c': { char c = (char)va_arg(ap, int); pad_out(out, ctx, &c, 1, width, ' ', left); break; }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int len = 0;
            while (s[len]) len++;
            pad_out(out, ctx, s, len, width, ' ', left);
            break;
        }
        case 'd': {
            int32_t v = va_arg(ap, int32_t);
            n = utoa_radix(v < 0 ? (uint32_t)0 - (uint32_t)v : (uint32_t)v, RADIX_DEC, rev);
            if (v < 0) tmp[k++] = '-';
            while (n) tmp[k++] = rev[--n];
            pad_out(out, ctx, tmp, k, width, left ? ' ' : pad, left);
            break;
        }
        case 'u':
        case 'x':
        case 'X': {
            n = utoa_radix(va_arg(ap, uint32_t), *fmt == 'u' ? RADIX_DEC : RADIX_HEX, rev);
            while (n) {
                char ch = rev[--n];
                if (*fmt == 'X' && ch >= 'a' && ch <= 'f') ch = (char)(ch - 32);
                tmp[k++] = ch;
            }
            pad_out(out, ctx, tmp, k, width, left ? ' ' : pad, left);
            break;
        }
        case 'p': {
            n = utoa_radix((uint32_t)(uintptr_t)va_arg(ap, void *), RADIX_HEX, rev);
            out('0', ctx); out('x', ctx);
            while (n) tmp[k++] = rev[--n];
            pad_out(out, ctx, tmp, k, 8, '0', false);
            break;
        }
        case '%': out('%', ctx); break;
        case 0:   return;
        default:  out('%', ctx); out(*fmt, ctx); break;
        }
    }
}

/* ---- console output ---------------------------------------------------------- */

/* One mutex serializes whole kputs()/kprintf() calls, so concurrent tasks
 * produce intact lines.  It is taken only in plain task context: interrupt
 * handlers, masked sections and the panic path print without it (they cannot
 * block), accepting possible interleaving there. */
static mutex_t s_console_lock = MUTEX_INIT;

static bool console_lock(void)
{
    if (sched_started() && arch_irq_enabled()) { mutex_lock(&s_console_lock); return true; }
    return false;
}
static void console_unlock(bool locked) { if (locked) mutex_unlock(&s_console_lock); }

/* Everything printed is also kept in a small ring so a GUI terminal can show it,
 * and the console can be fed characters as if typed (see kprintf.h). */
#define CONSOLE_LOG_SIZE  2048u
#define CONSOLE_IN_SIZE   256u

static char     s_log[CONSOLE_LOG_SIZE];
static uint32_t s_log_total;                              /* characters ever logged */
static char     s_in[CONSOLE_IN_SIZE];
static uint32_t s_in_head, s_in_tail;

static void con_out(char c, void *ctx)
{
    (void)ctx;
    uint32_t irq = arch_irq_disable();
    s_log[s_log_total++ % CONSOLE_LOG_SIZE] = c;
    arch_irq_restore(irq);
    if (c == '\n') hal_console_putc('\r');            /* terminals want CR LF */
    hal_console_putc(c);
}

uint32_t kconsole_log_end(void) { return s_log_total; }

size_t kconsole_log_read(uint32_t *pos, char *buf, size_t max)
{
    uint32_t irq = arch_irq_disable();
    if (s_log_total - *pos > CONSOLE_LOG_SIZE) *pos = s_log_total - CONSOLE_LOG_SIZE;   /* we fell behind: skip the lost part */
    size_t n = s_log_total - *pos;
    if (n > max) n = max;
    for (size_t i = 0; i < n; i++) buf[i] = s_log[(*pos + i) % CONSOLE_LOG_SIZE];
    *pos += (uint32_t)n;
    arch_irq_restore(irq);
    return n;
}

size_t kconsole_inject_n(const char *s, size_t n)
{
    size_t done = 0;
    uint32_t irq = arch_irq_disable();
    for (; done < n; done++) {
        if (((s_in_head + 1) % CONSOLE_IN_SIZE) == s_in_tail) break;           /* full: the caller keeps the rest */
        s_in[s_in_head] = s[done];
        s_in_head = (s_in_head + 1) % CONSOLE_IN_SIZE;
    }
    arch_irq_restore(irq);
    return done;
}

void kconsole_inject(const char *s)
{
    size_t n = 0;
    while (s[n]) n++;
    kconsole_inject_n(s, n);                                                    /* full: drop the rest */
}

/* Input focus: a full-screen app that owns the display gets the keys; everyone else reads nothing meanwhile. */
static volatile uint32_t s_focus_task;
void kconsole_set_focus(uint32_t task_id) { s_focus_task = task_id; }

static volatile bool s_refresh;
void kconsole_request_refresh(void) { s_refresh = true; }
bool kconsole_take_refresh(void) { bool r = s_refresh; s_refresh = false; return r; }

int kconsole_getc(void)
{
    if (s_focus_task && task_current() && task_current()->id != s_focus_task) return -1;
    int c = -1;
    uint32_t irq = arch_irq_disable();
    if (s_in_tail != s_in_head) { c = (uint8_t)s_in[s_in_tail]; s_in_tail = (s_in_tail + 1) % CONSOLE_IN_SIZE; }
    arch_irq_restore(irq);
    return c >= 0 ? c : hal_console_getc();
}

void kputs(const char *s)
{
    bool l = console_lock();
    while (*s) con_out(*s++, NULL);
    hal_console_flush();
    console_unlock(l);
}

void kconsole_write(const char *buf, size_t n)
{
    bool l = console_lock();
    for (size_t i = 0; i < n; i++) con_out(buf[i], NULL);
    hal_console_flush();
    console_unlock(l);
}

void kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    bool l = console_lock();
    vformat(con_out, NULL, fmt, ap);
    hal_console_flush();
    console_unlock(l);
    va_end(ap);
}

/* ---- formatting into a buffer ------------------------------------------------ */

struct buf_ctx { char *buf; size_t cap, len; };

static void buf_out(char c, void *ctx)
{
    struct buf_ctx *b = ctx;
    if (b->len + 1 < b->cap) b->buf[b->len] = c;
    b->len++;
}

int ksnprintf(char *buf, size_t cap, const char *fmt, ...)
{
    struct buf_ctx b = { buf, cap, 0 };
    va_list ap;
    va_start(ap, fmt);
    vformat(buf_out, &b, fmt, ap);
    va_end(ap);
    if (cap) buf[b.len < cap ? b.len : cap - 1] = 0;
    return (int)(b.len < cap ? b.len : cap - 1);
}
