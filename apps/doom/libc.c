#include <m5os/board_config.h>
#if M5OS_DOOM
#include <m5os/kmalloc.h>
#include <m5os/kprintf.h>
#include "runtime.h"
#include "../../components/doomengine/compat.h"

typedef union { struct { size_t size; } h; uint64_t align[2]; } allocation_t;
void *doom_malloc(size_t n)
{
    if (!n) n = 1;
    if (n > SIZE_MAX - sizeof(allocation_t)) doom_exit(1);
    allocation_t *p = kmalloc_psram_owned(n + sizeof *p, doom_owner);
    if (!p) { kputs("doom: PSRAM allocation failed\n"); doom_exit(1); }
    p->h.size = n; return p + 1;
}
void doom_free(void *p) { if (p) kfree((allocation_t *)p - 1); }
void *doom_calloc(size_t n, size_t size)
{
    if (size && n > SIZE_MAX / size) doom_exit(1);
    void *p = doom_malloc(n * size); memset(p, 0, n * size); return p;
}
void *doom_realloc(void *p, size_t n)
{
    if (!p) return doom_malloc(n);
    if (!n) { doom_free(p); return NULL; }
    size_t old = ((allocation_t *)p - 1)->h.size;
    void *next = doom_malloc(n); memcpy(next, p, old < n ? old : n); doom_free(p); return next;
}
char *doom_strdup(const char *s) { size_t n = strlen(s) + 1; char *p = doom_malloc(n); memcpy(p, s, n); return p; }

typedef struct { size_t pos; bool error; } memory_file_t;
FILE *doom_fopen(const char *path, const char *mode)
{
    /* Only the preloaded IWAD is exposed; no config/save writes or arbitrary host files. */
    if (!doom_iwad || strcmp(path, doom_iwad_path) || (strcmp(mode, "r") && strcmp(mode, "rb"))) return NULL;
    memory_file_t *f = doom_calloc(1, sizeof *f); return (FILE *)f;
}
static memory_file_t *stream(FILE *f) { return (uintptr_t)f > 3 ? (memory_file_t *)f : NULL; }
int doom_fclose(FILE *f) { if (!stream(f)) return EOF; doom_free(f); return 0; }
size_t doom_fread(void *buf, size_t size, size_t count, FILE *file)
{
    memory_file_t *f = stream(file);
    if (!f || !size || count > SIZE_MAX / size) return 0;
    size_t n = size * count, left = doom_iwad_size - f->pos;
    if (n > left) n = left;
    memcpy(buf, doom_iwad + f->pos, n); f->pos += n; return n / size;
}
size_t doom_fwrite(const void *buf, size_t size, size_t count, FILE *f)
{
    (void)buf; (void)size; (void)count; memory_file_t *s = stream(f); if (s) s->error = true; return 0;
}
int doom_fseek(FILE *file, long offset, int whence)
{
    memory_file_t *f = stream(file); if (!f) return -1;
    int64_t pos = whence == SEEK_SET ? offset : whence == SEEK_CUR ? (int64_t)f->pos + offset :
                  whence == SEEK_END ? (int64_t)doom_iwad_size + offset : -1;
    if (pos < 0 || (uint64_t)pos > doom_iwad_size) { f->error = true; return -1; }
    f->pos = pos; return 0;
}
long doom_ftell(FILE *f) { memory_file_t *s = stream(f); return s ? (long)s->pos : -1; }
int doom_feof(FILE *f) { memory_file_t *s = stream(f); return !s || s->pos >= doom_iwad_size; }
int doom_ferror(FILE *f) { memory_file_t *s = stream(f); return !s || s->error; }
int doom_fgetc(FILE *f) { uint8_t b; return doom_fread(&b, 1, 1, f) == 1 ? b : EOF; }
int doom_fputc(int c, FILE *f) { (void)c; (void)f; return EOF; }
int doom_fflush(FILE *f) { (void)f; return 0; }
char *doom_getenv(const char *name) { (void)name; return NULL; }
int doom_mkdir(const char *path, mode_t mode) { (void)path; (void)mode; return -1; }
int doom_remove(const char *path) { (void)path; return -1; }
int doom_rename(const char *a, const char *b) { (void)a; (void)b; return -1; }
int doom_fileno(FILE *f) { (void)f; return -1; }
int doom_isatty(int fd) { (void)fd; return 0; }

/* Integer-only formatting, independent of newlib stdio/reentrancy/locks. */
static void emit(char *b, size_t cap, size_t *n, char c) { if (*n + 1 < cap) b[*n] = c; (*n)++; }
int doom_vsnprintf(char *b, size_t cap, const char *fmt, va_list args)
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
        } else if (type == 'f' || type == 'g') { (void)va_arg(args, double); s = "[float]"; count = 7; }
        else { temp[0] = '?'; count = 1; }
        if (!left) for (int i = (int)count; i < width; i++) emit(b, cap, &n, zero ? '0' : ' ');
        for (size_t i = 0; i < count; i++) emit(b, cap, &n, s[i]);
        if (left) for (int i = (int)count; i < width; i++) emit(b, cap, &n, ' ');
    }
    if (cap) b[n < cap ? n : cap - 1] = 0;
    return n > INT32_MAX ? INT32_MAX : (int)n;
}
int doom_snprintf(char *b, size_t cap, const char *fmt, ...) { va_list a; va_start(a, fmt); int n = doom_vsnprintf(b, cap, fmt, a); va_end(a); return n; }
int doom_sprintf(char *b, const char *fmt, ...) { va_list a; va_start(a, fmt); int n = doom_vsnprintf(b, SIZE_MAX, fmt, a); va_end(a); return n; }
int doom_vfprintf(FILE *f, const char *fmt, va_list a)
{
    if (f != stdout && f != stderr) return -1;
    char b[512]; int n = doom_vsnprintf(b, sizeof b, fmt, a); kputs(b); return n;
}
int doom_fprintf(FILE *f, const char *fmt, ...) { va_list a; va_start(a, fmt); int n = doom_vfprintf(f, fmt, a); va_end(a); return n; }
int doom_printf(const char *fmt, ...) { va_list a; va_start(a, fmt); int n = doom_vfprintf(stdout, fmt, a); va_end(a); return n; }
int doom_puts(const char *s) { kputs(s); kputs("\n"); return 0; }
int doom_putchar(int c) { char b[2] = { (char)c, 0 }; kputs(b); return c; }
int doom_fscanf(FILE *f, const char *fmt, ...) { (void)f; (void)fmt; return EOF; } /* config files are not exposed */
int doom_sscanf(const char *s, const char *fmt, ...)
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
void doom_assertion(const char *file, int line, const char *fn, const char *condition)
{
    kprintf("doom: assert %s:%d %s %s\n", file, line, fn, condition); doom_exit(1);
}
#endif
