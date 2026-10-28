#pragma once
/* UTF-8 <-> UCS-2 helpers shared by the input method.  Header-only and free of OS headers so the
 * conversion engine can be built and tested on the host (tools/ime/host_test.sh). */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

static inline int ime_u8_enc(uint32_t cp, char *o)
{
    if (cp < 0x80u)  { o[0] = (char)cp; return 1; }
    if (cp < 0x800u) { o[0] = (char)(0xC0u | (cp >> 6)); o[1] = (char)(0x80u | (cp & 0x3Fu)); return 2; }
    o[0] = (char)(0xE0u | (cp >> 12)); o[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu)); o[2] = (char)(0x80u | (cp & 0x3Fu));
    return 3;
}

/* Decodes one code point (BMP only; anything longer yields U+FFFD).  Returns the bytes consumed, >= 1. */
static inline int ime_u8_dec(const char *s, uint32_t *cp)
{
    const uint8_t *p = (const uint8_t *)s;
    if (p[0] < 0x80u) { *cp = p[0]; return 1; }
    if ((p[0] & 0xE0u) == 0xC0u && (p[1] & 0xC0u) == 0x80u) { *cp = ((uint32_t)(p[0] & 0x1Fu) << 6) | (p[1] & 0x3Fu); return 2; }
    if ((p[0] & 0xF0u) == 0xE0u && (p[1] & 0xC0u) == 0x80u && (p[2] & 0xC0u) == 0x80u) {
        *cp = ((uint32_t)(p[0] & 0x0Fu) << 12) | ((uint32_t)(p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu);
        return 3;
    }
    *cp = 0xFFFDu;
    return 1;
}

/* NUL-terminated UTF-8 out of n code points; returns the length in bytes (output is truncated to cap-1). */
static inline int ime_cps_to_u8(const uint16_t *cps, int n, char *out, int cap)
{
    int len = 0;
    for (int i = 0; i < n; i++) {
        char t[3];
        int k = ime_u8_enc(cps[i], t);
        if (len + k >= cap) break;
        for (int j = 0; j < k; j++) out[len++] = t[j];
    }
    out[len] = 0;
    return len;
}

/* UTF-8 to code points; returns how many were stored (at most cap). */
static inline int ime_u8_to_cps(const char *s, uint16_t *out, int cap)
{
    int n = 0;
    while (*s && n < cap) {
        uint32_t cp;
        s += ime_u8_dec(s, &cp);
        out[n++] = (uint16_t)cp;
    }
    return n;
}

static inline bool ime_is_hira(uint16_t c) { return (c >= 0x3041u && c <= 0x3096u) || c == 0x30FCu; }   /* ー belongs to words */
static inline uint16_t ime_to_kata(uint16_t c) { return (c >= 0x3041u && c <= 0x3096u) ? (uint16_t)(c + 0x60u) : c; }
static inline uint16_t ime_to_hira(uint16_t c) { return (c >= 0x30A1u && c <= 0x30F6u) ? (uint16_t)(c - 0x60u) : c; }
