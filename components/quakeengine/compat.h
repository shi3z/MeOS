#include <sys/types.h>
#pragma once
#include "esp_attr.h"
#undef EXT_RAM_BSS_ATTR
#define EXT_RAM_BSS_ATTR
#include <assert.h>
/* Included only in the Doom engine's translation units. Never redirects MeOS libc globally. */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdint.h>
#include "profile.h"

#ifndef QUAKE_COMPAT_DECLARATIONS_ONLY
#undef stdin
#undef stdout
#undef stderr
#define stdout ((FILE *)(uintptr_t)1)
#define stderr ((FILE *)(uintptr_t)2)
#define stdin ((FILE *)(uintptr_t)3)
#endif

double quake_atof(const char *s);
void *quake_malloc(size_t n);
void *quake_calloc(size_t n, size_t size);
void *quake_realloc(void *p, size_t n);
void quake_free(void *p);
char *quake_strdup(const char *s);
FILE *quake_fopen(const char *path, const char *mode);
int quake_fclose(FILE *f);
size_t quake_fread(void *buf, size_t size, size_t count, FILE *f);
size_t quake_fwrite(const void *buf, size_t size, size_t count, FILE *f);
int quake_fseek(FILE *f, long off, int whence);
long quake_ftell(FILE *f);
int quake_feof(FILE *f);
int quake_ferror(FILE *f);
int quake_fgetc(FILE *f);
int quake_fputc(int c, FILE *f);
int quake_fflush(FILE *f);
int quake_fprintf(FILE *f, const char *fmt, ...);
int quake_vfprintf(FILE *f, const char *fmt, va_list ap);
int quake_printf(const char *fmt, ...);
int quake_puts(const char *s);
int quake_putchar(int c);
int quake_snprintf(char *b, size_t n, const char *fmt, ...);
int quake_vsnprintf(char *b, size_t n, const char *fmt, va_list ap);
int quake_sprintf(char *b, const char *fmt, ...);
int quake_sscanf(const char *s, const char *fmt, ...);
int quake_fscanf(FILE *f, const char *fmt, ...);
char *quake_getenv(const char *name);
int quake_mkdir(const char *path, mode_t mode);
int quake_remove(const char *path);
int quake_rename(const char *old, const char *new_name);
int quake_fileno(FILE *f);
int quake_isatty(int fd);
void quake_exit(int code) __attribute__((noreturn));
void quake_assertion(const char *file, int line, const char *function, const char *condition) __attribute__((noreturn));

#ifndef QUAKE_COMPAT_DECLARATIONS_ONLY
#undef assert
#define assert(test) ((test) ? (void)0 : quake_assertion(__FILE__,__LINE__,__func__,#test))
#define atof quake_atof
#define malloc quake_malloc
#define calloc quake_calloc
#define realloc quake_realloc
#define free quake_free
#define strdup quake_strdup
#define fopen quake_fopen
#define fclose quake_fclose
#define fread quake_fread
#define fwrite quake_fwrite
#define fseek quake_fseek
#define ftell quake_ftell
#define feof quake_feof
#define ferror quake_ferror
#undef getc
#undef putc
#define getc quake_fgetc
#define putc quake_fputc
#define fgetc quake_fgetc
#define fputc quake_fputc
#define fflush quake_fflush
#define fprintf quake_fprintf
#define vfprintf quake_vfprintf
#define printf quake_printf
#define puts quake_puts
#define putchar quake_putchar
#define snprintf quake_snprintf
#define vsnprintf quake_vsnprintf
#define sprintf quake_sprintf
#define sscanf quake_sscanf
#define fscanf quake_fscanf
#define getenv quake_getenv
#define mkdir quake_mkdir
#define remove quake_remove
#define rename quake_rename
#define fileno quake_fileno
#define isatty quake_isatty
#define exit quake_exit
#define __assert_func quake_assertion
#endif
