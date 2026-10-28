#pragma once
#include <m5os/types.h>

/* Minimal formatted output (%c %s %d %u %x %p %%, optional width, 0-pad, '-' left-justify).
 * Own implementation on purpose: newlib's printf takes a FreeRTOS-backed
 * lock (see IDF newlib/locks.c) and we must not touch FreeRTOS. */
void kputs(const char *s);
/* GCC's printf-format check assumes uint32_t is "unsigned int"; that holds on Xtensa but not on RISC-V
 * (newlib: "unsigned long"), where it would reject every %u of a uint32_t although both are 32 bits and
 * our formatter treats them the same.  So the check stays on for Xtensa builds only. */
#ifdef __XTENSA__
#  define M5_PRINTF_CHECK(fmt_idx, arg_idx)  __attribute__((format(printf, fmt_idx, arg_idx)))
#else
#  define M5_PRINTF_CHECK(fmt_idx, arg_idx)
#endif

void kprintf(const char *fmt, ...) M5_PRINTF_CHECK(1, 2);

/* Format into a buffer (always NUL-terminated, truncates); returns the stored length. */
int ksnprintf(char *buf, size_t cap, const char *fmt, ...) M5_PRINTF_CHECK(3, 4);

/* Raw byte output to the console (no formatting); serialized like kputs. */
void kconsole_write(const char *buf, size_t n);

/* The console keeps the last ~2 KB it printed, readable by position: a viewer keeps its own
 * `pos` (start from kconsole_log_end()) and calls kconsole_log_read() for what is new.  If it
 * falls too far behind, the lost part is skipped. */
uint32_t kconsole_log_end(void);
size_t   kconsole_log_read(uint32_t *pos, char *buf, size_t max);

/* Feed characters to whoever reads the console (the shell) as if they had been typed. */
void     kconsole_inject(const char *s);
size_t   kconsole_inject_n(const char *s, size_t n);   /* returns how many bytes fitted; the rest was not queued */
void     kconsole_set_focus(uint32_t task_id);   /* 0 = nobody: only this task reads the console (see wasm display_open) */
/* A program that owns the screen is asked to repaint it (the on-screen keyboard covered part of it). */
void     kconsole_request_refresh(void);
bool     kconsole_take_refresh(void);          /* true once per request */
int      kconsole_getc(void);                 /* injected characters first, then the serial port; -1 if none */
