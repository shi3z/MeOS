#pragma once
#include <stddef.h>
#include "../../third_party/ubasic/ubasic.h"
#define BASIC_LINE_CAP 128
#define BASIC_MAX_LINES 128
#define BASIC_PROGRAM_CAP (BASIC_MAX_LINES * (BASIC_LINE_CAP + 8))
typedef struct { unsigned number; char text[BASIC_LINE_CAP]; } basic_line;
typedef struct { basic_line lines[BASIC_MAX_LINES]; unsigned count; } basic_program;
enum { BASIC_LOAD, BASIC_SAVE, BASIC_EDIT };
/* 0: complete NUL-terminated line; >0: truncated/invalid line (retry);
 * -1: user cancelled; other negative: input unavailable/read failure. */
typedef int (*basic_readline)(void *opaque, char *buffer, size_t capacity);
typedef int (*basic_file_command)(void *opaque, int action, const char *path);
/* One active shell session; interpreter itself is single-instance. */
typedef struct {
    basic_program source;
    char program[BASIC_PROGRAM_CAP];
    ubasic_output output;
    void *opaque;
    int (*cancel)(void *);
    basic_file_command file;
    basic_readline input;
    ubasic_graphics graphics;
} basic_session;
void basic_session_init(basic_session *, ubasic_output, int (*cancel)(void *), void *opaque);
/* Returns 1 for BYE, otherwise 0. Input excludes newline; errors use output callback. */
int basic_session_submit(basic_session *, const char *line);

/* Export canonical numbered LF text into session->program; returns byte length. */
size_t basic_session_export(basic_session *);
/* Parse file bytes into separate staging storage; no statements are executed.
 * Returns NULL on success, otherwise an error and physical one-based input line.
 * Accepts LF, CRLF, CR, optional UTF-8 BOM and blank lines; rejects duplicate
 * numbers, NUL/control bytes and oversized input. Caller commits staging only
 * after success. BASIC statement syntax remains checked at execution time. */
const char *basic_program_parse(basic_program *staging, const char *data, size_t length, unsigned *error_line);
/* Execute stored source, resetting variables. 0 success, -1 error, -2 break. */
int basic_session_run(basic_session *);
