#include <m5os/board_config.h>
#if M5OS_QUAKE
#include <m5os/kmalloc.h>
#include <m5os/kprintf.h>
#include "runtime.h"
#include <m5os/vfs.h>
#include <m5os/number.h>
#include "../../components/quakeengine/compat.h"

typedef union { struct { size_t size; } h; uint64_t align[2]; } allocation_t;
void *quake_malloc(size_t n)
{
    if (!n) n = 1;
    if (n > SIZE_MAX - sizeof(allocation_t)) quake_exit(1);
    allocation_t *p = kmalloc_psram_owned(n + sizeof *p, quake_owner);
    if (!p) { kputs("quake: PSRAM allocation failed\n"); quake_exit(1); }
    p->h.size = n; return p + 1;
}
void quake_free(void *p) { if (p) kfree((allocation_t *)p - 1); }
void *quake_calloc(size_t n, size_t size)
{
    if (size && n > SIZE_MAX / size) quake_exit(1);
    void *p = quake_malloc(n * size); memset(p, 0, n * size); return p;
}
void *quake_realloc(void *p, size_t n)
{
    if (!p) return quake_malloc(n);
    if (!n) { quake_free(p); return NULL; }
    size_t old = ((allocation_t *)p - 1)->h.size;
    void *next = quake_malloc(n); memcpy(next, p, old < n ? old : n); quake_free(p); return next;
}
char *quake_strdup(const char *s) { size_t n = strlen(s) + 1; char *p = quake_malloc(n); memcpy(p, s, n); return p; }

#include "io.inc"
char *quake_getenv(const char *name) { (void)name; return NULL; }
int quake_mkdir(const char *path, mode_t mode) { (void)path; (void)mode; return -1; }
int quake_remove(const char *path) { (void)path; return -1; }
int quake_rename(const char *a, const char *b) { (void)a; (void)b; return -1; }
int quake_fileno(FILE *f) { (void)f; return -1; }
int quake_isatty(int fd) { (void)fd; return 0; }

/* Integer-only formatting, independent of newlib stdio/reentrancy/locks. */
static void emit(char *b, size_t cap, size_t *n, char c) { if (*n + 1 < cap) b[*n] = c; (*n)++; }
int quake_vsnprintf(char *b, size_t cap, const char *fmt, va_list args)
{
    size_t n = 0;
    while (*fmt) {
        if (*fmt != '%') { emit(b, cap, &n, *fmt++); continue; }
        fmt++; if (*fmt == '%') { emit(b, cap, &n, *fmt++); continue; }
        bool left = false, zero = false; int width = 0, precision = -1, length = 0;
        while (*fmt == '-' || *fmt == '0') { if (*fmt == '-') left = true; else zero = true; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + *fmt++ - '0'; if (width > 4096) width = 4096; }
        if (*fmt == '.') {
            fmt++; precision = 0;
            if (*fmt == '*') { precision = va_arg(args, int); fmt++; }
            else while (*fmt >= '0' && *fmt <= '9') { precision = precision * 10 + *fmt++ - '0'; if (precision > 4096) precision = 4096; }
        }
        if (*fmt == 'z') { length = 3; fmt++; }
        else while (*fmt == 'l') { length++; fmt++; }
        char temp[70]; const char *s = temp; size_t count = 0; char type = *fmt ? *fmt++ : 0;
        if (type == 's') { s = va_arg(args, const char *); if (!s) s = "(null)"; count = strlen(s); if (precision >= 0 && count > (size_t)precision) count = precision; }
        else if (type == 'c') { temp[0] = va_arg(args, int); count = 1; }
        else if (type == 'd' || type == 'i' || type == 'u' || type == 'x' || type == 'X' || type == 'o' || type == 'p') {
            uint64_t value; bool negative = false; unsigned base = type == 'o' ? 8 : (type == 'x' || type == 'X' || type == 'p') ? 16 : 10;
            if (type == 'p') value = (uintptr_t)va_arg(args, void *);
            else if (type == 'd' || type == 'i') {
                int64_t v = length == 2 ? va_arg(args, long long) : length == 1 ? va_arg(args, long) : length == 3 ? (int64_t)va_arg(args, ptrdiff_t) : va_arg(args, int);
                negative = v < 0; value = negative ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
            } else value = length == 2 ? va_arg(args, unsigned long long) : length == 1 ? va_arg(args, unsigned long) : length == 3 ? va_arg(args, size_t) : va_arg(args, unsigned int);
            char rev[65]; size_t k = 0;
            do { rev[k++] = "0123456789abcdef"[value % base]; value /= base; } while (value);
            if (negative) temp[count++] = '-';
            if (type == 'p') { temp[count++] = '0'; temp[count++] = 'x'; }
            /* Doom names HUD lumps with %.3d (e.g. STCFN033), not only %03d. */
            while (precision > (int)k && count < sizeof temp - k) { temp[count++] = '0'; precision--; }
            while (k) { char c = rev[--k]; if (type == 'X' && c >= 'a' && c <= 'f') c -= 32; temp[count++] = c; }
        }
        else if (type == 'f' || type == 'g' || type == 'e') {
            double value=va_arg(args,double); count=number_format(value,temp,sizeof temp);
            if(!count) { strcpy(temp,"nan"); count=3; }
        }
        else { temp[0] = '?'; count = 1; }
        if (!left) for (int i = (int)count; i < width; i++) emit(b, cap, &n, zero ? '0' : ' ');
        for (size_t i = 0; i < count; i++) emit(b, cap, &n, s[i]);
        if (left) for (int i = (int)count; i < width; i++) emit(b, cap, &n, ' ');
    }
    if (cap) b[n < cap ? n : cap - 1] = 0;
    return n > INT32_MAX ? INT32_MAX : (int)n;
}
int quake_snprintf(char *b, size_t cap, const char *fmt, ...) { va_list a; va_start(a, fmt); int n = quake_vsnprintf(b, cap, fmt, a); va_end(a); return n; }
int quake_sprintf(char *b, const char *fmt, ...) { va_list a; va_start(a, fmt); int n = quake_vsnprintf(b, SIZE_MAX, fmt, a); va_end(a); return n; }
int quake_vfprintf(FILE *f, const char *fmt, va_list a)
{
    if (f != stdout && f != stderr) return -1;
    char b[512]; int n = quake_vsnprintf(b, sizeof b, fmt, a); kputs(b); return n;
}
int quake_fprintf(FILE *f, const char *fmt, ...) { va_list a; va_start(a, fmt); int n = quake_vfprintf(f, fmt, a); va_end(a); return n; }
int quake_printf(const char *fmt, ...) { va_list a; va_start(a, fmt); int n = quake_vfprintf(stdout, fmt, a); va_end(a); return n; }
int quake_puts(const char *s) { kputs(s); kputs("\n"); return 0; }
int quake_putchar(int c) { char b[2] = { (char)c, 0 }; kputs(b); return c; }
int quake_fscanf(FILE *f, const char *fmt, ...) { (void)f; (void)fmt; return EOF; } /* config files are not exposed */
int quake_sscanf(const char *s, const char *fmt, ...)
{
    while (*s == ' ' || *s == '\t') s++;
    while (*fmt == ' ') fmt++;
    if (*fmt != '%') { while (*fmt && *fmt != '%') { if (*s++ != *fmt++) return 0; } }
    if (*fmt++ != '%') return 0;
    int base = *fmt == 'x' ? 16 : *fmt == 'o' ? 8 : (*fmt == 'd' || *fmt == 'i') ? 10 : 0;
    if (!base) return 0;
    bool neg = *s == '-'; if (*s == '-' || *s == '+') s++;
    if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    unsigned v = 0, n = 0;
    while (*s) { unsigned d = *s >= '0' && *s <= '9' ? *s - '0' : *s >= 'a' && *s <= 'f' ? *s - 'a' + 10 : *s >= 'A' && *s <= 'F' ? *s - 'A' + 10 : 99; if (d >= (unsigned)base) break; v = v * base + d; s++; n++; }
    if (!n) return 0;
    va_list a; va_start(a, fmt); *va_arg(a, int *) = neg ? (int)(0u - v) : (int)v; va_end(a); return 1;
}
void quake_assertion(const char *file, int line, const char *fn, const char *condition)
{
    kprintf("quake: assert %s:%d %s %s\n", file, line, fn, condition); quake_exit(1);
}
#endif
