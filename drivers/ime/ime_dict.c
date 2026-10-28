#include "ime_dict.h"
#include <string.h>

#define MAX_DICTS 4
#define LEARN_MAX 24
#define KEY_MAX   64
#define TEXT_MAX  64

typedef struct { const uint8_t *base; uint32_t size, count; } dict_t;
static dict_t s_dict[MAX_DICTS];
static int s_ndict;

typedef struct { char key[KEY_MAX]; char text[TEXT_MAX]; } learn_t;
static learn_t s_learn[LEARN_MAX];
static int s_nlearn;

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }

bool ime_dict_add(const uint8_t *blob, uint32_t size)
{
    if (!blob || size < IME_DICT_HEADER || s_ndict >= MAX_DICTS) return false;
    if (rd32(blob) != IME_DICT_MAGIC) return false;
    uint32_t count = rd32(blob + 4), total = rd32(blob + 8);
    if (total > size || IME_DICT_HEADER + 4ull * count > total) return false;
    s_dict[s_ndict].base = blob; s_dict[s_ndict].size = total; s_dict[s_ndict].count = count;
    s_ndict++;
    return true;
}

void ime_dict_clear(void) { s_ndict = 0; }

/* Entry for `key`, or NULL.  *data / *dlen describe the candidate block. */
static bool find(const dict_t *d, const char *key, size_t klen, const uint8_t **data, uint32_t *dlen)
{
    uint32_t lo = 0, hi = d->count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint32_t off = rd32(d->base + IME_DICT_HEADER + 4u * mid);
        if (off >= d->size) return false;
        const uint8_t *e = d->base + off;
        size_t el = e[0];
        size_t n = el < klen ? el : klen;
        int c = memcmp(e + 1, key, n);
        if (c == 0) c = (el > klen) - (el < klen);
        if (c == 0) {
            *dlen = rd16(e + 1 + el);
            *data = e + 3 + el;
            return off + 3 + el + *dlen <= d->size;
        }
        if (c < 0) lo = mid + 1; else hi = mid;
    }
    return false;
}

static bool has(const char *out, int len, const char *s)
{
    for (const char *p = out; p < out + len && *p; p += strlen(p) + 1)
        if (strcmp(p, s) == 0) return true;
    return false;
}

static int add(char *out, int *len, int cap, const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || has(out, *len, s)) return 0;
    if (*len + (int)n + 2 > cap) return -1;
    memcpy(out + *len, s, n + 1);
    *len += (int)n + 1;
    out[*len] = 0;
    return 1;
}

int ime_dict_lookup(const char *key, char *out, int cap)
{
    int len = 0, count = 0;
    if (cap < 2) return 0;
    out[0] = 0;
    size_t klen = strlen(key);
    for (int d = 0; d < s_ndict; d++) {
        const uint8_t *data;
        uint32_t dlen;
        if (!find(&s_dict[d], key, klen, &data, &dlen)) continue;
        for (uint32_t p = 0; p < dlen;) {
            const char *c = (const char *)data + p;
            size_t n = strnlen(c, dlen - p);
            if (p + n >= dlen) break;                                    /* unterminated: corrupt */
            int r = add(out, &len, cap, c);
            if (r < 0) return count;
            count += r;
            p += (uint32_t)n + 1;
        }
    }
    return count;
}

int ime_dict_learned(const char *key, char *out, int cap)
{
    int len = 0, count = 0;
    if (cap < 2) return 0;
    out[0] = 0;
    for (int i = s_nlearn - 1; i >= 0; i--)                       /* newest first */
        if (strcmp(s_learn[i].key, key) == 0) {
            int r = add(out, &len, cap, s_learn[i].text);
            if (r < 0) break;
            count += r;
        }
    return count;
}

void ime_dict_learn(const char *key, const char *text)
{
    if (strlen(key) >= KEY_MAX || strlen(text) >= TEXT_MAX) return;
    for (int i = 0; i < s_nlearn; i++)
        if (strcmp(s_learn[i].key, key) == 0 && strcmp(s_learn[i].text, text) == 0) {        /* known: make it the newest */
            learn_t t = s_learn[i];
            for (int j = i; j + 1 < s_nlearn; j++) s_learn[j] = s_learn[j + 1];
            s_learn[s_nlearn - 1] = t;
            return;
        }
    if (s_nlearn == LEARN_MAX) {
        for (int j = 0; j + 1 < LEARN_MAX; j++) s_learn[j] = s_learn[j + 1];
        s_nlearn--;
    }
    strcpy(s_learn[s_nlearn].key, key);
    strcpy(s_learn[s_nlearn].text, text);
    s_nlearn++;
}
