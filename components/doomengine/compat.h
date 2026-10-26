#include <sys/types.h>
#pragma once
/* Included only in the Doom engine's translation units. Never redirects MeOS libc globally. */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdint.h>

#ifndef DOOM_COMPAT_DECLARATIONS_ONLY
#undef stdin
#undef stdout
#undef stderr
#define stdout ((FILE *)(uintptr_t)1)
#define stderr ((FILE *)(uintptr_t)2)
#define stdin ((FILE *)(uintptr_t)3)
#endif

void *doom_malloc(size_t n);
void *doom_calloc(size_t n, size_t size);
void *doom_realloc(void *p, size_t n);
void doom_free(void *p);
char *doom_strdup(const char *s);
FILE *doom_fopen(const char *path, const char *mode);
int doom_fclose(FILE *f);
size_t doom_fread(void *buf, size_t size, size_t count, FILE *f);
size_t doom_fwrite(const void *buf, size_t size, size_t count, FILE *f);
int doom_fseek(FILE *f, long off, int whence);
long doom_ftell(FILE *f);
int doom_feof(FILE *f);
int doom_ferror(FILE *f);
int doom_fgetc(FILE *f);
int doom_fputc(int c, FILE *f);
int doom_fflush(FILE *f);
int doom_fprintf(FILE *f, const char *fmt, ...);
int doom_vfprintf(FILE *f, const char *fmt, va_list ap);
int doom_printf(const char *fmt, ...);
int doom_puts(const char *s);
int doom_putchar(int c);
int doom_snprintf(char *b, size_t n, const char *fmt, ...);
int doom_vsnprintf(char *b, size_t n, const char *fmt, va_list ap);
int doom_sprintf(char *b, const char *fmt, ...);
int doom_sscanf(const char *s, const char *fmt, ...);
int doom_fscanf(FILE *f, const char *fmt, ...);
char *doom_getenv(const char *name);
int doom_mkdir(const char *path, mode_t mode);
int doom_remove(const char *path);
int doom_rename(const char *old, const char *new_name);
int doom_fileno(FILE *f);
int doom_isatty(int fd);
void doom_exit(int code) __attribute__((noreturn));
void doom_assertion(const char *file, int line, const char *function, const char *condition) __attribute__((noreturn));

#ifndef DOOM_COMPAT_DECLARATIONS_ONLY
#define malloc doom_malloc
#define calloc doom_calloc
#define realloc doom_realloc
#define free doom_free
#define strdup doom_strdup
#define fopen doom_fopen
#define fclose doom_fclose
#define fread doom_fread
#define fwrite doom_fwrite
#define fseek doom_fseek
#define ftell doom_ftell
#define feof doom_feof
#define ferror doom_ferror
#define fgetc doom_fgetc
#define fputc doom_fputc
#define fflush doom_fflush
#define fprintf doom_fprintf
#define vfprintf doom_vfprintf
#define printf doom_printf
#define puts doom_puts
#define putchar doom_putchar
#define snprintf doom_snprintf
#define vsnprintf doom_vsnprintf
#define sprintf doom_sprintf
#define sscanf doom_sscanf
#define fscanf doom_fscanf
#define getenv doom_getenv
#define mkdir doom_mkdir
#define remove doom_remove
#define rename doom_rename
#define fileno doom_fileno
#define isatty doom_isatty
#define exit doom_exit
#define __assert_func doom_assertion
#endif
