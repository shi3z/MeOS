#include "buffer.h"
#include <stdint.h>
#include <string.h>

bool scheme_utf8_text(const char *s, size_t n)
{
    for (size_t i = 0; i < n;) {
        unsigned c = (unsigned char)s[i++];
        if (c < 128) {
            if (c < 32 && c != '\n' && c != '\t' && c != '\r')
                return false;
            if (c == 127)
                return false;
            continue;
        }
        unsigned need, cp, min;
        if (c >= 0xc2 && c <= 0xdf) {
            need = 1;
            cp = c & 31;
            min = 128;
        } else if (c >= 0xe0 && c <= 0xef) {
            need = 2;
            cp = c & 15;
            min = 2048;
        } else if (c >= 0xf0 && c <= 0xf4) {
            need = 3;
            cp = c & 7;
            min = 65536;
        } else
            return false;
        if (n - i < need)
            return false;
        while (need--) {
            unsigned b = (unsigned char)s[i++];
            if ((b & 0xc0) != 0x80)
                return false;
            cp = (cp << 6) | (b & 63);
        }
        if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
            return false;
    }
    return true;
}
static void snapshot(scheme_buffer *b)
{
    memcpy(b->undo, b->text, b->length + 1);
    b->undo_length = b->length;
    b->undo_cursor = b->cursor;
    b->can_undo = true;
}
bool scheme_buffer_set(scheme_buffer *b, const char *s, size_t n)
{
    if (n > SCHEME_SOURCE_CAP || !scheme_utf8_text(s, n))
        return false;
    memmove(b->text, s, n);
    b->text[n] = 0;
    b->length = n;
    b->cursor = 0;
    b->modified = b->can_undo = false;
    return true;
}
bool scheme_buffer_insert(scheme_buffer *b, const char *s, size_t n)
{
    if (n > SCHEME_SOURCE_CAP - b->length || !scheme_utf8_text(s, n))
        return false;
    snapshot(b);
    memmove(b->text + b->cursor + n, b->text + b->cursor, b->length - b->cursor + 1);
    memcpy(b->text + b->cursor, s, n);
    b->length += n;
    b->cursor += n;
    b->modified = true;
    return true;
}
size_t scheme_buffer_prev(const scheme_buffer *b, size_t p)
{
    if (!p)
        return 0;
    p--;
    while (p && ((unsigned char)b->text[p] & 0xc0) == 0x80)
        p--;
    return p;
}
size_t scheme_buffer_next(const scheme_buffer *b, size_t p)
{
    if (p >= b->length)
        return b->length;
    p++;
    while (p < b->length && ((unsigned char)b->text[p] & 0xc0) == 0x80)
        p++;
    return p;
}
bool scheme_buffer_delete(scheme_buffer *b, bool backward)
{
    size_t start = backward ? scheme_buffer_prev(b, b->cursor) : b->cursor;
    size_t end = backward ? b->cursor : scheme_buffer_next(b, b->cursor);
    if (start == end)
        return false;
    snapshot(b);
    memmove(b->text + start, b->text + end, b->length - end + 1);
    b->length -= end - start;
    b->cursor = start;
    b->modified = true;
    return true;
}
void scheme_buffer_undo(scheme_buffer *b)
{
    if (!b->can_undo)
        return;
    memcpy(b->text, b->undo, b->undo_length + 1);
    b->length = b->undo_length;
    b->cursor = b->undo_cursor;
    b->can_undo = false;
    b->modified = true;
}

static bool form_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
bool scheme_buffer_previous_form(const scheme_buffer *b, size_t point, size_t *begin, size_t *end)
{
    if (point > b->length || !scheme_utf8_text(b->text, point))
        return false;
    size_t stack[64], depth = 0, quote = SIZE_MAX;
    size_t first = 0, last = 0;
    bool complete = false;
    for (size_t p = 0; p < point;) {
        char c = b->text[p];
        if (form_space(c)) {
            p++;
            continue;
        }
        if (c == ';') {
            while (p < point && b->text[p] != '\n')
                p++;
            continue;
        }
        if (c == '\'') {
            if (quote == SIZE_MAX)
                quote = p;
            complete = false;
            p++;
            continue;
        }
        size_t from = quote == SIZE_MAX ? p : quote;
        if (c == '(') {
            if (depth == sizeof stack / sizeof stack[0])
                return false;
            stack[depth++] = from;
            quote = SIZE_MAX;
            complete = false;
            p++;
            continue;
        }
        if (c == ')') {
            if (!depth || quote != SIZE_MAX)
                return false;
            first = stack[--depth];
            last = ++p;
            complete = true;
            continue;
        }
        if (c == '"') {
            bool closed = false;
            p++;
            while (p < point) {
                c = b->text[p++];
                if (c == '\\') {
                    if (p == point)
                        return false;
                    p++;
                } else if (c == '"') {
                    closed = true;
                    break;
                }
            }
            if (!closed)
                return false;
        } else {
            while (p < point && !form_space(b->text[p]) && b->text[p] != '(' && b->text[p] != ')' &&
                   b->text[p] != ';') {
                if (b->text[p] == '"')
                    return false;
                p++;
            }
        }
        first = from;
        last = p;
        quote = SIZE_MAX;
        complete = true;
    }
    if (!complete || quote != SIZE_MAX)
        return false;
    *begin = first;
    *end = last;
    return true;
}
