#pragma once
#include <stdbool.h>
#include <stddef.h>
#define SCHEME_SOURCE_CAP 16384
typedef struct {
    char text[SCHEME_SOURCE_CAP + 1], undo[SCHEME_SOURCE_CAP + 1];
    size_t length, cursor, undo_length, undo_cursor;
    bool modified, can_undo;
} scheme_buffer;
bool scheme_utf8_text(const char *, size_t);
bool scheme_buffer_set(scheme_buffer *, const char *, size_t);
bool scheme_buffer_insert(scheme_buffer *, const char *, size_t);
bool scheme_buffer_delete(scheme_buffer *, bool backward);
void scheme_buffer_undo(scheme_buffer *);
size_t scheme_buffer_prev(const scheme_buffer *, size_t);
size_t scheme_buffer_next(const scheme_buffer *, size_t);
/* Span of the complete expression immediately before point. Enclosing lists
 * may still be open. Strings, comments and quote prefixes are respected. */
bool scheme_buffer_previous_form(const scheme_buffer *, size_t point, size_t *begin, size_t *end);
