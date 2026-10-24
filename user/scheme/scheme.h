#pragma once
#include <stdbool.h>
#include <stddef.h>

/* Bounded Scheme subset, independent of the kernel and libc I/O. Caller owns
 * aligned workspace. Not an R7RS implementation; see README.md in this
 * directory. */
typedef struct scheme scheme;
typedef struct {
    void *context;
    void (*write)(void *, const char *);
    void (*plot)(void *, const double *, const double *, unsigned, const char *title);
    bool (*cancel)(void *);
    bool (*insert_text)(void *, const char *);
} scheme_host;
size_t scheme_workspace_size(void);
scheme *scheme_init(void *workspace, scheme_host host);
/* Parse everything before executing. last_only evaluates the final top-level
 * form (for C-x C-e); previous forms are read but not executed. */
bool scheme_eval(scheme *, const char *source, bool last_only, char *result, size_t capacity);
size_t scheme_error_offset(const scheme *);
/* Visit global zero-argument user procedures without allocating or evaluating.
 * Callback must not mutate the interpreter. Names remain owned by the VM. */
void scheme_visit_commands(const scheme *, void (*visit)(void *, const char *), void *context);
