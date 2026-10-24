/*
 * MeOS Tailscale Client & WireGuard / Disco Virtual Network Stack
 *
 * Implements:
 *  - Tailscale ts2021 Control Protocol over TCP port 80 (controlplane.tailscale.com:80)
 *    - HTTP Upgrade ("tailscale-control-protocol") + X-Tailscale-Handshake
 *    - Noise_IK_25519_ChaChaPoly_BLAKE2s (Protocol Version 109)
 *    - EarlyPayloadMagic ("\xff\xff\xffTS" + 4B length + JSON challenge)
 *    - HTTP/2 framing (/machine/register & /machine/map)
 *  - Tailscale Disco ("TS\xf0\x9f\x92\xac") over UDP :41641 (X25519 + XSalsa20-Poly1305 Ping/Pong)
 *  - WireGuard (Noise_IKpsk2_25519_ChaChaPoly_BLAKE2s, Type 1/2/4) over UDP :41641
 *  - Virtual 100.64.0.0/10 tunnel & MagicDNS hostname resolution for MeOS TCP/ICMP (e.g. `ssh`)
 *  - Both CLI (`tailscale up|status|ip|ping|down`) and Graphical Tailscale Manager (`tailscale`)
 */
#include "tailscale_app.h"
#include "../tui/dqwin.h"
#include "../wifi/wifi_app.h"
#include "../../drivers/lcd/fbcon.h"

#include <m5os/wifi.h>
#include <m5os/display.h>
#include <m5os/kprintf.h>
#include <m5os/hal_time.h>
#include <m5os/task.h>
#include <m5os/mutex.h>
#include <m5os/vfs.h>
#include <m5os/kmalloc.h>

#include "esp_random.h"
#include "esp_mac.h"


/* =========================================================================
 * Basic Memory / String Helpers
 * ========================================================================= */

static void mem_zero(void *dst, size_t n)
{
    volatile uint8_t *p = (volatile uint8_t *)dst;
    for (size_t i = 0; i < n; i++) p[i] = 0;
}

static void mem_copy(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
}

static bool mem_eq(const void *a, const void *b, size_t n)
{
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    uint8_t diff = 0;
    for (size_t i = 0; i < n; i++) diff |= (pa[i] ^ pb[i]);
    return diff == 0;
}

static size_t str_len(const char *s)
{
    size_t n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

static void str_copy(char *dst, size_t max, const char *src)
{
    if (!dst || max == 0) return;
    size_t i = 0;
    if (src) {
        for (; src[i] && i < max - 1; i++) dst[i] = src[i];
    }
    dst[i] = 0;
}

static bool str_eq(const char *a, const char *b)
{
    if (!a || !b) return false;
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static char to_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static bool str_ieq(const char *a, const char *b)
{
    if (!a || !b) return false;
    while (*a && to_lower(*a) == to_lower(*b)) { a++; b++; }
    return *a == *b;
}

static bool str_starts_with(const char *s, const char *prefix)
{
    if (!s || !prefix) return false;
    while (*prefix) {
        if (*s++ != *prefix++) return false;
    }
    return true;
}

static const char *str_find(const char *hay, const char *needle)
{
    if (!hay || !needle || !needle[0]) return NULL;
    size_t nlen = str_len(needle);
    for (size_t i = 0; hay[i]; i++) {
        bool match = true;
        for (size_t j = 0; j < nlen; j++) {
            if (hay[i + j] != needle[j]) { match = false; break; }
        }
        if (match) return &hay[i];
    }
    return NULL;
}

static void rng_bytes(uint8_t *out, size_t len)
{
    esp_fill_random(out, len);
}

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void put_le64(uint8_t *p, uint64_t v)
{
    put_le32(p, (uint32_t)(v & 0xFFFFFFFFULL));
    put_le32(p + 4, (uint32_t)(v >> 32));
}

static uint32_t rotr32(uint32_t x, int n)
{
    return (x >> n) | (x << (32 - n));
}

static uint32_t rotl32(uint32_t x, int n)
{
    return (x << n) | (x >> (32 - n));
}

static void bytes_to_hex(const uint8_t *in, size_t len, char *out)
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2]     = hex[in[i] >> 4];
        out[i * 2 + 1] = hex[in[i] & 0x0Fu];
    }
    out[len * 2] = 0;
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool hex_to_bytes(const char *hex, uint8_t *out, size_t out_len)
{
    if (!hex || !out) return false;
    for (size_t i = 0; i < out_len; i++) {
        int hi = hex_val(hex[i * 2]);
        int lo = hex_val(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static void base64_encode(const uint8_t *in, size_t len, char *out)
{
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i = 0, j = 0;
    while (i < len) {
        uint32_t a = i < len ? in[i++] : 0;
        uint32_t b = i < len ? in[i++] : 0;
        uint32_t c = i < len ? in[i++] : 0;
        uint32_t triple = (a << 16) | (b << 8) | c;
        out[j++] = b64[(triple >> 18) & 0x3Fu];
        out[j++] = b64[(triple >> 12) & 0x3Fu];
        out[j++] = b64[(triple >> 6)  & 0x3Fu];
        out[j++] = b64[triple & 0x3Fu];
    }
    size_t mod = len % 3u;
    if (mod == 1u) { out[j - 1] = '='; out[j - 2] = '='; }
    else if (mod == 2u) { out[j - 1] = '='; }
    out[j] = 0;
}

/* =========================================================================
 * 1. BLAKE2s (RFC 7693) + HMAC-BLAKE2s + HKDF-BLAKE2s
 * ========================================================================= */

static const uint32_t s_blake2s_iv[8] = {
    0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au,
    0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u
};

static const uint8_t s_blake2s_sigma[10][16] = {
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15 },
    {14,10, 4, 8, 9,15,13, 6, 1,12, 0, 2,11, 7, 5, 3 },
    {11, 8,12, 0, 5, 2,15,13,10,14, 3, 6, 7, 1, 9, 4 },
    { 7, 9, 3, 1,13,12,11,14, 2, 6, 5,10, 4, 0,15, 8 },
    { 9, 0, 5, 7, 2, 4,10,15,14, 1,11,12, 6, 8, 3,13 },
    { 2,12, 6,10, 0,11, 8, 3, 4,13, 7, 5,15,14, 1, 9 },
    {12, 5, 1,15,14,13, 4,10, 0, 7, 6, 3, 9, 2, 8,11 },
    {13,11, 7,14,12, 1, 3, 9, 5, 0,15, 4, 8, 6, 2,10 },
    { 6,15,14, 9,11, 3, 0, 8,12, 2,13, 7, 1, 4,10, 5 },
    {10, 2, 8, 4, 7, 6, 1, 5,15,11, 9,14, 3,12,13, 0 }
};

typedef struct {
    uint32_t h[8];
    uint32_t t[2];
    uint8_t  buf[64];
    size_t   buflen;
    size_t   outlen;
} blake2s_ctx_t;

#define B2S_G(v, a, b, c, d, x, y) do { \
    v[a] = v[a] + v[b] + (x); \
    v[d] = rotr32(v[d] ^ v[a], 16); \
    v[c] = v[c] + v[d]; \
    v[b] = rotr32(v[b] ^ v[c], 12); \
    v[a] = v[a] + v[b] + (y); \
    v[d] = rotr32(v[d] ^ v[a], 8); \
    v[c] = v[c] + v[d]; \
    v[b] = rotr32(v[b] ^ v[c], 7); \
} while (0)

static void blake2s_compress(blake2s_ctx_t *ctx, bool last)
{
    uint32_t v[16], m[16];
    for (int i = 0; i < 8; i++) {
        v[i]     = ctx->h[i];
        v[i + 8] = s_blake2s_iv[i];
    }
    v[12] ^= ctx->t[0];
    v[13] ^= ctx->t[1];
    if (last) v[14] = ~v[14];
    for (int i = 0; i < 16; i++) m[i] = get_le32(&ctx->buf[i * 4]);

    for (int r = 0; r < 10; r++) {
        const uint8_t *s = s_blake2s_sigma[r];
        B2S_G(v, 0, 4,  8, 12, m[s[0]],  m[s[1]]);
        B2S_G(v, 1, 5,  9, 13, m[s[2]],  m[s[3]]);
        B2S_G(v, 2, 6, 10, 14, m[s[4]],  m[s[5]]);
        B2S_G(v, 3, 7, 11, 15, m[s[6]],  m[s[7]]);
        B2S_G(v, 0, 5, 10, 15, m[s[8]],  m[s[9]]);
        B2S_G(v, 1, 6, 11, 12, m[s[10]], m[s[11]]);
        B2S_G(v, 2, 7,  8, 13, m[s[12]], m[s[13]]);
        B2S_G(v, 3, 4,  9, 14, m[s[14]], m[s[15]]);
    }
    for (int i = 0; i < 8; i++) ctx->h[i] ^= v[i] ^ v[i + 8];
}

static void blake2s_init(blake2s_ctx_t *ctx, size_t outlen, const void *key, size_t keylen)
{
    mem_zero(ctx, sizeof(*ctx));
    for (int i = 0; i < 8; i++) ctx->h[i] = s_blake2s_iv[i];
    ctx->h[0] ^= 0x01010000u ^ ((uint32_t)keylen << 8) ^ (uint32_t)outlen;
    ctx->outlen = outlen;
    if (keylen > 0 && key) {
        mem_copy(ctx->buf, key, keylen);
        ctx->buflen = 64u;
    }
}

static void blake2s_update(blake2s_ctx_t *ctx, const void *in, size_t inlen)
{
    const uint8_t *p = (const uint8_t *)in;
    while (inlen > 0) {
        if (ctx->buflen == 64u) {
            ctx->t[0] += 64u;
            if (ctx->t[0] < 64u) ctx->t[1]++;
            blake2s_compress(ctx, false);
            ctx->buflen = 0;
        }
        size_t take = 64u - ctx->buflen;
        if (take > inlen) take = inlen;
        mem_copy(&ctx->buf[ctx->buflen], p, take);
        ctx->buflen += take;
        p += take;
        inlen -= take;
    }
}

static void blake2s_final(blake2s_ctx_t *ctx, uint8_t *out)
{
    ctx->t[0] += (uint32_t)ctx->buflen;
    if (ctx->t[0] < (uint32_t)ctx->buflen) ctx->t[1]++;
    while (ctx->buflen < 64u) ctx->buf[ctx->buflen++] = 0;
    blake2s_compress(ctx, true);
    uint8_t tmp[32];
    for (int i = 0; i < 8; i++) put_le32(&tmp[i * 4], ctx->h[i]);
    mem_copy(out, tmp, ctx->outlen);
}

static void blake2s_hash(uint8_t out[32], const void *in, size_t inlen)
{
    blake2s_ctx_t ctx;
    blake2s_init(&ctx, 32, NULL, 0);
    blake2s_update(&ctx, in, inlen);
    blake2s_final(&ctx, out);
}

static void blake2s_hash2(uint8_t out[32], const void *a, size_t alen, const void *b, size_t blen)
{
    blake2s_ctx_t ctx;
    blake2s_init(&ctx, 32, NULL, 0);
    blake2s_update(&ctx, a, alen);
    blake2s_update(&ctx, b, blen);
    blake2s_final(&ctx, out);
}

static void blake2s_mac16(uint8_t out[16], const uint8_t key[32], const void *in, size_t inlen)
{
    blake2s_ctx_t ctx;
    blake2s_init(&ctx, 16, key, 32);
    blake2s_update(&ctx, in, inlen);
    blake2s_final(&ctx, out);
}

static void hmac_blake2s(uint8_t out[32], const uint8_t *key, size_t keylen, const void *data, size_t datalen)
{
    uint8_t k0[64];
    mem_zero(k0, 64);
    if (keylen > 64u) {
        blake2s_hash(k0, key, keylen);
    } else if (keylen > 0) {
        mem_copy(k0, key, keylen);
    }
    uint8_t ipad[64], opad[64], inner[32];
    for (int i = 0; i < 64; i++) {
        ipad[i] = k0[i] ^ 0x36u;
        opad[i] = k0[i] ^ 0x5Cu;
    }
    blake2s_ctx_t ctx;
    blake2s_init(&ctx, 32, NULL, 0);
    blake2s_update(&ctx, ipad, 64);
    blake2s_update(&ctx, data, datalen);
    blake2s_final(&ctx, inner);

    blake2s_init(&ctx, 32, NULL, 0);
    blake2s_update(&ctx, opad, 64);
    blake2s_update(&ctx, inner, 32);
    blake2s_final(&ctx, out);
}

static void hkdf_blake2s_2(uint8_t t1[32], uint8_t t2[32], const uint8_t ck[32], const uint8_t *ikm, size_t ikm_len)
{
    uint8_t prk[32];
    hmac_blake2s(prk, ck, 32, ikm, ikm_len);
    uint8_t b1 = 0x01u;
    hmac_blake2s(t1, prk, 32, &b1, 1);
    if (t2) {
        uint8_t m2[33];
        mem_copy(m2, t1, 32);
        m2[32] = 0x02u;
        hmac_blake2s(t2, prk, 32, m2, 33);
    }
}

static void hkdf_blake2s_3(uint8_t t1[32], uint8_t t2[32], uint8_t t3[32], const uint8_t ck[32], const uint8_t *ikm, size_t ikm_len)
{
    uint8_t prk[32];
    hmac_blake2s(prk, ck, 32, ikm, ikm_len);
    uint8_t b1 = 0x01u;
    hmac_blake2s(t1, prk, 32, &b1, 1);
    uint8_t m[33];
    mem_copy(m, t1, 32);
    m[32] = 0x02u;
    hmac_blake2s(t2, prk, 32, m, 33);
    mem_copy(m, t2, 32);
    m[32] = 0x03u;
    hmac_blake2s(t3, prk, 32, m, 33);
}

/* =========================================================================
 * 2. X25519 (RFC 7748)
 * ========================================================================= */

typedef int64_t fe25519[16];

static void fe_zero(fe25519 out)
{
    for (int i = 0; i < 16; i++) out[i] = 0;
}

static void fe_copy(fe25519 out, const fe25519 in)
{
    for (int i = 0; i < 16; i++) out[i] = in[i];
}

static void fe_add(fe25519 out, const fe25519 a, const fe25519 b)
{
    for (int i = 0; i < 16; i++) out[i] = a[i] + b[i];
}

static void fe_sub(fe25519 out, const fe25519 a, const fe25519 b)
{
    for (int i = 0; i < 16; i++) out[i] = a[i] - b[i];
}

static void fe_car25519(fe25519 o)
{
    for (int i = 0; i < 16; i++) {
        o[i] += (1LL << 16);
        int64_t c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c << 16;
    }
}

static void fe_mul(fe25519 o, const fe25519 a, const fe25519 b)
{
    int64_t t[31];
    for (int i = 0; i < 31; i++) t[i] = 0;
    for (int i = 0; i < 16; i++) {
        for (int j = 0; j < 16; j++) {
            t[i + j] += a[i] * b[j];
        }
    }
    for (int i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; i++) o[i] = t[i];
    fe_car25519(o);
    fe_car25519(o);
}

static void fe_sq(fe25519 o, const fe25519 a)
{
    fe_mul(o, a, a);
}

static void fe_inv25519(fe25519 o, const fe25519 i)
{
    fe25519 c;
    fe_copy(c, i);
    for (int a = 253; a >= 0; a--) {
        fe_sq(c, c);
        if (a != 2 && a != 4) fe_mul(c, c, i);
    }
    fe_copy(o, c);
}

static void fe_sel25519(fe25519 p, fe25519 q, int b)
{
    int64_t c = ~(int64_t)(b - 1);
    for (int i = 0; i < 16; i++) {
        int64_t t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

static void fe_unpack25519(fe25519 o, const uint8_t n[32])
{
    for (int i = 0; i < 16; i++) {
        o[i] = (int64_t)n[2 * i] + ((int64_t)n[2 * i + 1] << 8);
    }
    o[15] &= 0x7fff;
}

static void fe_pack25519(uint8_t o[32], const fe25519 n)
{
    fe25519 m, t;
    fe_copy(t, n);
    fe_car25519(t);
    fe_car25519(t);
    fe_car25519(t);
    for (int j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int b = (int)((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        fe_sel25519(t, m, 1 - b);
    }
    for (int i = 0; i < 16; i++) {
        o[2 * i]     = (uint8_t)(t[i] & 0xff);
        o[2 * i + 1] = (uint8_t)(t[i] >> 8);
    }
}

static void x25519_scalarmult(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32])
{
    uint8_t z[32];
    mem_copy(z, scalar, 32);
    z[0]  &= 248u;
    z[31] = (z[31] & 127u) | 64u;

    fe25519 x, a, b, c, d, e, f, c121665;
    fe_unpack25519(x, point);
    fe_zero(a); a[0] = 1;
    fe_copy(b, x);
    fe_zero(c);
    fe_zero(d); d[0] = 1;
    fe_zero(c121665); c121665[0] = 0xDB41; c121665[1] = 1;

    for (int i = 254; i >= 0; i--) {
        int r = (z[i >> 3] >> (i & 7)) & 1;
        fe_sel25519(a, b, r);
        fe_sel25519(c, d, r);
        fe_add(e, a, c);
        fe_sub(a, a, c);
        fe_add(c, b, d);
        fe_sub(b, b, d);
        fe_sq(d, e);
        fe_sq(f, a);
        fe_mul(a, c, a);
        fe_mul(c, b, e);
        fe_add(e, a, c);
        fe_sub(a, a, c);
        fe_sq(b, a);
        fe_sub(c, d, f);
        fe_mul(a, c, c121665);
        fe_add(a, a, d);
        fe_mul(c, c, a);
        fe_mul(a, d, f);
        fe_mul(d, b, x);
        fe_sq(b, e);
        fe_sel25519(a, b, r);
        fe_sel25519(c, d, r);
    }
    fe_inv25519(c, c);
    fe_mul(a, a, c);
    fe_pack25519(out, a);
}

static void x25519_base(uint8_t pub[32], const uint8_t priv[32])
{
    static const uint8_t base9[32] = {9};
    x25519_scalarmult(pub, priv, base9);
}

/* =========================================================================
 * 3. ChaCha20-Poly1305 (RFC 8439) + Poly1305 MAC
 * ========================================================================= */

#define CHACHA_QR(a, b, c, d) do { \
    a += b; d ^= a; d = rotl32(d, 16); \
    c += d; b ^= c; b = rotl32(b, 12); \
    a += b; d ^= a; d = rotl32(d, 8);  \
    c += d; b ^= c; b = rotl32(b, 7);  \
} while (0)

static void chacha20_block(uint8_t out[64], const uint8_t key[32], uint32_t counter, const uint8_t nonce[12])
{
    uint32_t s[16], x[16];
    s[0] = 0x61707865u; s[1] = 0x3320646eu; s[2] = 0x79622d32u; s[3] = 0x6b206574u;
    for (int i = 0; i < 8; i++) s[4 + i] = get_le32(&key[i * 4]);
    s[12] = counter;
    s[13] = get_le32(&nonce[0]);
    s[14] = get_le32(&nonce[4]);
    s[15] = get_le32(&nonce[8]);

    for (int i = 0; i < 16; i++) x[i] = s[i];
    for (int i = 0; i < 10; i++) {
        CHACHA_QR(x[0], x[4], x[8],  x[12]);
        CHACHA_QR(x[1], x[5], x[9],  x[13]);
        CHACHA_QR(x[2], x[6], x[10], x[14]);
        CHACHA_QR(x[3], x[7], x[11], x[15]);
        CHACHA_QR(x[0], x[5], x[10], x[15]);
        CHACHA_QR(x[1], x[6], x[11], x[12]);
        CHACHA_QR(x[2], x[7], x[8],  x[13]);
        CHACHA_QR(x[3], x[4], x[9],  x[14]);
    }
    for (int i = 0; i < 16; i++) put_le32(&out[i * 4], x[i] + s[i]);
}

static void chacha20_xor(uint8_t *dst, const uint8_t *src, size_t len,
                         const uint8_t key[32], uint32_t counter, const uint8_t nonce[12])
{
    uint8_t block[64];
    size_t off = 0;
    while (off < len) {
        chacha20_block(block, key, counter++, nonce);
        size_t chunk = (len - off > 64u) ? 64u : (len - off);
        for (size_t i = 0; i < chunk; i++) dst[off + i] = src[off + i] ^ block[i];
        off += chunk;
    }
}

typedef struct {
    uint32_t r[5];
    uint32_t pad[4];
    uint32_t h[5];
    uint8_t  buf[16];
    size_t   buflen;
} poly1305_ctx_t;

static void poly1305_init(poly1305_ctx_t *ctx, const uint8_t key[32])
{
    mem_zero(ctx, sizeof(*ctx));
    ctx->r[0] = (get_le32(&key[0])) & 0x3ffffffu;
    ctx->r[1] = (get_le32(&key[3]) >> 2) & 0x3ffff03u;
    ctx->r[2] = (get_le32(&key[6]) >> 4) & 0x3ffc0ffu;
    ctx->r[3] = (get_le32(&key[9]) >> 6) & 0x3f03fffu;
    ctx->r[4] = (get_le32(&key[12]) >> 8) & 0x00fffffu;
    for (int i = 0; i < 4; i++) ctx->pad[i] = get_le32(&key[16 + i * 4]);
}

static void poly1305_blocks(poly1305_ctx_t *ctx, const uint8_t *m, size_t bytes, uint32_t hibit)
{
    uint32_t r0 = ctx->r[0], r1 = ctx->r[1], r2 = ctx->r[2], r3 = ctx->r[3], r4 = ctx->r[4];
    uint32_t s1 = r1 * 5u, s2 = r2 * 5u, s3 = r3 * 5u, s4 = r4 * 5u;
    uint32_t h0 = ctx->h[0], h1 = ctx->h[1], h2 = ctx->h[2], h3 = ctx->h[3], h4 = ctx->h[4];

    while (bytes >= 16u) {
        h0 += (get_le32(&m[0])) & 0x3ffffffu;
        h1 += (get_le32(&m[3]) >> 2) & 0x3ffffffu;
        h2 += (get_le32(&m[6]) >> 4) & 0x3ffffffu;
        h3 += (get_le32(&m[9]) >> 6) & 0x3ffffffu;
        h4 += (get_le32(&m[12]) >> 8) | (hibit << 24);

        uint64_t d0 = (uint64_t)h0*r0 + (uint64_t)h1*s4 + (uint64_t)h2*s3 + (uint64_t)h3*s2 + (uint64_t)h4*s1;
        uint64_t d1 = (uint64_t)h0*r1 + (uint64_t)h1*r0 + (uint64_t)h2*s4 + (uint64_t)h3*s3 + (uint64_t)h4*s2;
        uint64_t d2 = (uint64_t)h0*r2 + (uint64_t)h1*r1 + (uint64_t)h2*r0 + (uint64_t)h3*s4 + (uint64_t)h4*s3;
        uint64_t d3 = (uint64_t)h0*r3 + (uint64_t)h1*r2 + (uint64_t)h2*r1 + (uint64_t)h3*r0 + (uint64_t)h4*s4;
        uint64_t d4 = (uint64_t)h0*r4 + (uint64_t)h1*r3 + (uint64_t)h2*r2 + (uint64_t)h3*r1 + (uint64_t)h4*r0;

        uint32_t c;
        c = (uint32_t)(d0 >> 26); h0 = (uint32_t)d0 & 0x3ffffffu; d1 += c;
        c = (uint32_t)(d1 >> 26); h1 = (uint32_t)d1 & 0x3ffffffu; d2 += c;
        c = (uint32_t)(d2 >> 26); h2 = (uint32_t)d2 & 0x3ffffffu; d3 += c;
        c = (uint32_t)(d3 >> 26); h3 = (uint32_t)d3 & 0x3ffffffu; d4 += c;
        c = (uint32_t)(d4 >> 26); h4 = (uint32_t)d4 & 0x3ffffffu; h0 += c * 5u;
        c = h0 >> 26; h0 &= 0x3ffffffu; h1 += c;

        m += 16u;
        bytes -= 16u;
    }
    ctx->h[0] = h0; ctx->h[1] = h1; ctx->h[2] = h2; ctx->h[3] = h3; ctx->h[4] = h4;
}

static void poly1305_update(poly1305_ctx_t *ctx, const uint8_t *m, size_t bytes)
{
    if (ctx->buflen > 0) {
        size_t want = 16u - ctx->buflen;
        if (want > bytes) want = bytes;
        mem_copy(&ctx->buf[ctx->buflen], m, want);
        m += want;
        bytes -= want;
        ctx->buflen += want;
        if (ctx->buflen < 16u) return;
        poly1305_blocks(ctx, ctx->buf, 16, 1);
        ctx->buflen = 0;
    }
    if (bytes >= 16u) {
        size_t want = bytes & ~(size_t)15u;
        poly1305_blocks(ctx, m, want, 1);
        m += want;
        bytes -= want;
    }
    if (bytes > 0) {
        mem_copy(ctx->buf, m, bytes);
        ctx->buflen = bytes;
    }
}

static void poly1305_final(poly1305_ctx_t *ctx, uint8_t mac[16])
{
    if (ctx->buflen > 0) {
        size_t i = ctx->buflen;
        ctx->buf[i++] = 1u;
        while (i < 16u) ctx->buf[i++] = 0u;
        poly1305_blocks(ctx, ctx->buf, 16, 0);
    }

    uint32_t h0 = ctx->h[0], h1 = ctx->h[1], h2 = ctx->h[2], h3 = ctx->h[3], h4 = ctx->h[4];
    uint32_t c;
    c = h1 >> 26; h1 &= 0x3ffffffu; h2 += c;
    c = h2 >> 26; h2 &= 0x3ffffffu; h3 += c;
    c = h3 >> 26; h3 &= 0x3ffffffu; h4 += c;
    c = h4 >> 26; h4 &= 0x3ffffffu; h0 += c * 5u;
    c = h0 >> 26; h0 &= 0x3ffffffu; h1 += c;

    uint32_t g0 = h0 + 5u; c = g0 >> 26; g0 &= 0x3ffffffu;
    uint32_t g1 = h1 + c;  c = g1 >> 26; g1 &= 0x3ffffffu;
    uint32_t g2 = h2 + c;  c = g2 >> 26; g2 &= 0x3ffffffu;
    uint32_t g3 = h3 + c;  c = g3 >> 26; g3 &= 0x3ffffffu;
    uint32_t g4 = h4 + c - (1u << 26);

    uint32_t mask = (g4 >> 31) - 1u;
    g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0;
    h1 = (h1 & mask) | g1;
    h2 = (h2 & mask) | g2;
    h3 = (h3 & mask) | g3;
    h4 = (h4 & mask) | g4;

    h0 = (h0 | (h1 << 26));
    h1 = ((h1 >> 6) | (h2 << 20));
    h2 = ((h2 >> 12) | (h3 << 14));
    h3 = ((h3 >> 18) | (h4 << 8));

    uint64_t f;
    f = (uint64_t)h0 + ctx->pad[0];                h0 = (uint32_t)f;
    f = (uint64_t)h1 + ctx->pad[1] + (f >> 32);    h1 = (uint32_t)f;
    f = (uint64_t)h2 + ctx->pad[2] + (f >> 32);    h2 = (uint32_t)f;
    f = (uint64_t)h3 + ctx->pad[3] + (f >> 32);    h3 = (uint32_t)f;

    put_le32(&mac[0],  h0);
    put_le32(&mac[4],  h1);
    put_le32(&mac[8],  h2);
    put_le32(&mac[12], h3);
}

static void chachapoly_compute_tag(uint8_t tag[16], const uint8_t poly_key[32],
                                   const uint8_t *aad, size_t aad_len,
                                   const uint8_t *ct, size_t ct_len)
{
    static const uint8_t zeros[16] = {0};
    poly1305_ctx_t pctx;
    poly1305_init(&pctx, poly_key);
    if (aad_len > 0 && aad) {
        poly1305_update(&pctx, aad, aad_len);
        if (aad_len & 15u) poly1305_update(&pctx, zeros, 16u - (aad_len & 15u));
    }
    if (ct_len > 0 && ct) {
        poly1305_update(&pctx, ct, ct_len);
        if (ct_len & 15u) poly1305_update(&pctx, zeros, 16u - (ct_len & 15u));
    }
    uint8_t lens[16];
    put_le64(&lens[0], aad_len);
    put_le64(&lens[8], ct_len);
    poly1305_update(&pctx, lens, 16);
    poly1305_final(&pctx, tag);
}

static void chachapoly_seal(uint8_t *out, const uint8_t key[32], const uint8_t nonce[12],
                            const uint8_t *pt, size_t pt_len, const uint8_t *aad, size_t aad_len)
{
    uint8_t block0[64];
    chacha20_block(block0, key, 0, nonce);
    if (pt_len > 0) chacha20_xor(out, pt, pt_len, key, 1, nonce);
    chachapoly_compute_tag(out + pt_len, block0, aad, aad_len, out, pt_len);
}

static bool chachapoly_open(uint8_t *out, const uint8_t key[32], const uint8_t nonce[12],
                            const uint8_t *ct_and_tag, size_t total_len, const uint8_t *aad, size_t aad_len)
{
    if (total_len < 16u) return false;
    size_t ct_len = total_len - 16u;
    uint8_t block0[64], expected_tag[16];
    chacha20_block(block0, key, 0, nonce);
    chachapoly_compute_tag(expected_tag, block0, aad, aad_len, ct_and_tag, ct_len);
    if (!mem_eq(expected_tag, ct_and_tag + ct_len, 16)) return false;
    if (ct_len > 0 && out) chacha20_xor(out, ct_and_tag, ct_len, key, 1, nonce);
    return true;
}

static void noise_nonce_be(uint8_t nonce[12], uint64_t n)
{
    mem_zero(nonce, 4);
    for (int i = 0; i < 8; i++) nonce[4 + i] = (uint8_t)((n >> ((7 - i) * 8)) & 0xFFu);
}

static void wg_nonce_le(uint8_t nonce[12], uint64_t n)
{
    mem_zero(nonce, 4);
    put_le64(&nonce[4], n);
}

/* =========================================================================
 * 4. HSalsa20 + XSalsa20-Poly1305 (NaCl crypto_box for Tailscale Disco)
 * ========================================================================= */

#define SALSA_QR(a, b, c, d) do { \
    b ^= rotl32(a + d, 7);  \
    c ^= rotl32(b + a, 9);  \
    d ^= rotl32(c + b, 13); \
    a ^= rotl32(d + c, 18); \
} while (0)

static void hsalsa20(uint8_t out[32], const uint8_t key[32], const uint8_t in16[16])
{
    uint32_t x[16];
    x[0]  = 0x61707865u;
    x[5]  = 0x3320646eu;
    x[10] = 0x79622d32u;
    x[15] = 0x6b206574u;
    for (int i = 0; i < 4; i++) {
        x[1 + i]  = get_le32(&key[i * 4]);
        x[11 + i] = get_le32(&key[16 + i * 4]);
        x[6 + i]  = get_le32(&in16[i * 4]);
    }
    for (int i = 0; i < 10; i++) {
        SALSA_QR(x[0], x[4], x[8],  x[12]);
        SALSA_QR(x[5], x[9], x[13], x[1]);
        SALSA_QR(x[10],x[14],x[2],  x[6]);
        SALSA_QR(x[15],x[3], x[7],  x[11]);
        SALSA_QR(x[0], x[1], x[2],  x[3]);
        SALSA_QR(x[5], x[6], x[7],  x[4]);
        SALSA_QR(x[10],x[11],x[8],  x[9]);
        SALSA_QR(x[15],x[12],x[13], x[14]);
    }
    put_le32(&out[0],  x[0]);
    put_le32(&out[4],  x[5]);
    put_le32(&out[8],  x[10]);
    put_le32(&out[12], x[15]);
    put_le32(&out[16], x[6]);
    put_le32(&out[20], x[7]);
    put_le32(&out[24], x[8]);
    put_le32(&out[28], x[9]);
}

static void salsa20_block(uint8_t out[64], const uint8_t key[32], const uint8_t nonce8[8], uint64_t counter)
{
    uint32_t s[16], x[16];
    s[0]  = 0x61707865u;
    s[5]  = 0x3320646eu;
    s[10] = 0x79622d32u;
    s[15] = 0x6b206574u;
    for (int i = 0; i < 4; i++) {
        s[1 + i]  = get_le32(&key[i * 4]);
        s[11 + i] = get_le32(&key[16 + i * 4]);
    }
    s[6] = get_le32(&nonce8[0]);
    s[7] = get_le32(&nonce8[4]);
    s[8] = (uint32_t)(counter & 0xFFFFFFFFULL);
    s[9] = (uint32_t)(counter >> 32);

    for (int i = 0; i < 16; i++) x[i] = s[i];
    for (int i = 0; i < 10; i++) {
        SALSA_QR(x[0], x[4], x[8],  x[12]);
        SALSA_QR(x[5], x[9], x[13], x[1]);
        SALSA_QR(x[10],x[14],x[2],  x[6]);
        SALSA_QR(x[15],x[3], x[7],  x[11]);
        SALSA_QR(x[0], x[1], x[2],  x[3]);
        SALSA_QR(x[5], x[6], x[7],  x[4]);
        SALSA_QR(x[10],x[11],x[8],  x[9]);
        SALSA_QR(x[15],x[12],x[13], x[14]);
    }
    for (int i = 0; i < 16; i++) put_le32(&out[i * 4], x[i] + s[i]);
}

static void nacl_box_shared_key(uint8_t shared[32], const uint8_t my_priv[32], const uint8_t peer_pub[32])
{
    static const uint8_t zero16[16] = {0};
    uint8_t dh[32];
    x25519_scalarmult(dh, my_priv, peer_pub);
    hsalsa20(shared, dh, zero16);
}

static void xsalsa20_poly1305_seal(uint8_t *out, const uint8_t shared_key[32], const uint8_t nonce24[24],
                                   const uint8_t *pt, size_t pt_len)
{
    uint8_t subkey[32], block0[64];
    hsalsa20(subkey, shared_key, nonce24);
    salsa20_block(block0, subkey, &nonce24[16], 0);

    uint8_t *ct = out + 16;
    size_t pos = 0;
    size_t first = (pt_len > 32u) ? 32u : pt_len;
    for (size_t i = 0; i < first; i++) ct[i] = pt[i] ^ block0[32 + i];
    pos += first;

    uint64_t ctr = 1;
    uint8_t blk[64];
    while (pos < pt_len) {
        salsa20_block(blk, subkey, &nonce24[16], ctr++);
        size_t take = (pt_len - pos > 64u) ? 64u : (pt_len - pos);
        for (size_t i = 0; i < take; i++) ct[pos + i] = pt[pos + i] ^ blk[i];
        pos += take;
    }

    poly1305_ctx_t pctx;
    poly1305_init(&pctx, block0);
    poly1305_update(&pctx, ct, pt_len);
    poly1305_final(&pctx, out);
}

static bool xsalsa20_poly1305_open(uint8_t *out, const uint8_t shared_key[32], const uint8_t nonce24[24],
                                   const uint8_t *box, size_t box_len)
{
    if (box_len < 16u) return false;
    size_t ct_len = box_len - 16u;
    const uint8_t *ct = box + 16;

    uint8_t subkey[32], block0[64], expected[16];
    hsalsa20(subkey, shared_key, nonce24);
    salsa20_block(block0, subkey, &nonce24[16], 0);

    poly1305_ctx_t pctx;
    poly1305_init(&pctx, block0);
    poly1305_update(&pctx, ct, ct_len);
    poly1305_final(&pctx, expected);
    if (!mem_eq(expected, box, 16)) return false;

    size_t pos = 0;
    size_t first = (ct_len > 32u) ? 32u : ct_len;
    for (size_t i = 0; i < first; i++) out[i] = ct[i] ^ block0[32 + i];
    pos += first;

    uint64_t ctr = 1;
    uint8_t blk[64];
    while (pos < ct_len) {
        salsa20_block(blk, subkey, &nonce24[16], ctr++);
        size_t take = (ct_len - pos > 64u) ? 64u : (ct_len - pos);
        for (size_t i = 0; i < take; i++) out[pos + i] = ct[pos + i] ^ blk[i];
        pos += take;
    }
    return true;
}

/* =========================================================================
 * 5. Tailscale Node State, Persistent Keys (/home/tailscale.cfg), & Peer Table
 * ========================================================================= */

#define TS_SOCK_CTRL      3   /* Dedicated TCP socket index for controlplane.tailscale.com:80 */
#define TS_WG_PORT        41641u

/* Official controlplane.tailscale.com static Noise public key (protocolVersion = 109) */
static const uint8_t s_control_pub[32] = {
    0x7d,0x27,0x92,0xf9,0xc9,0x8d,0x75,0x3d,0x20,0x42,0x47,0x15,0x36,0x80,0x19,0x49,
    0x10,0x4c,0x24,0x7f,0x95,0xea,0xc7,0x70,0xf8,0xfb,0x32,0x15,0x95,0xe2,0x17,0x3b
};

typedef struct {
    ts_peer_info_t info;
    char           alt_hostname[48];
    uint8_t        disco_shared[32];
    /* WireGuard per-peer session state */
    bool           wg_hs_pending;
    uint32_t       my_index;
    uint32_t       peer_index;
    uint8_t        hs_e_priv[32];
    uint8_t        hs_ck[32];
    uint8_t        hs_h[32];
    uint8_t        tx_key[32];
    uint8_t        rx_key[32];
    uint64_t       tx_counter;
    uint64_t       last_hs_tx_us;
    uint64_t       last_rx_us;
    uint64_t       last_disco_tx_us;
} ts_peer_slot_t;

static mutex_t        s_ts_lock;
static bool           s_ts_lock_inited;
static uint32_t       s_ts_lock_depth;
static bool           s_keys_loaded;
static uint8_t        s_machine_priv[32];
static uint8_t        s_machine_pub[32];
static uint8_t        s_node_priv[32];
static uint8_t        s_node_pub[32];
static uint8_t        s_disco_priv[32];
static uint8_t        s_disco_pub[32];
static char           s_saved_authkey[128];

static ts_state_t     s_state = TS_STATE_STOPPED;
static uint8_t        s_my_ip[4];
static char           s_hostname[32] = "meos-tab5";
static char           s_auth_url[160];
static char           s_last_msg[80] = "Stopped";
static int            s_peer_count;
static ts_peer_slot_t s_peers[TAILSCALE_MAX_PEERS];

static volatile bool  s_daemon_running;
static volatile bool  s_want_up;
static uint32_t       s_next_wg_index = 0x100u;

/* Ping tracking state */
static volatile bool     s_ping_waiting;
static volatile bool     s_ping_done;
static uint64_t          s_ping_tx_us;
static uint32_t          s_ping_rtt_ms;
static uint8_t           s_ping_txid[12];
static uint16_t          s_ping_icmp_seq = 1u;
static uint8_t           s_ping_target_ip[4];
static uint8_t           s_ping_pong_ip[4];
static uint16_t          s_ping_pong_port;

static void ts_ensure_lock(void)
{
    if (!s_ts_lock_inited) {
        mutex_init(&s_ts_lock);
        s_ts_lock_inited = true;
    }
}

static void ts_lock(void)
{
    ts_ensure_lock();
    if (s_ts_lock.owner == task_current()) {
        s_ts_lock_depth++;
        return;
    }
    mutex_lock(&s_ts_lock);
    s_ts_lock_depth = 1u;
}

static void ts_unlock(void)
{
    if (s_ts_lock.owner == task_current()) {
        if (s_ts_lock_depth > 1u) {
            s_ts_lock_depth--;
            return;
        }
        s_ts_lock_depth = 0u;
        mutex_unlock(&s_ts_lock);
    }
}

static uint32_t ts_unlock_all(void)
{
    if (s_ts_lock.owner != task_current()) return 0u;
    uint32_t d = s_ts_lock_depth;
    s_ts_lock_depth = 0u;
    mutex_unlock(&s_ts_lock);
    return d;
}

static void ts_relock_all(uint32_t d)
{
    if (d == 0u) return;
    ts_ensure_lock();
    if (s_ts_lock.owner == task_current()) {
        s_ts_lock_depth += d;
        return;
    }
    mutex_lock(&s_ts_lock);
    s_ts_lock_depth = d;
}

static void ts_set_msg(const char *msg)
{
    str_copy(s_last_msg, sizeof(s_last_msg), msg);
}

static void clamp_x25519_priv(uint8_t k[32])
{
    k[0]  &= 248u;
    k[31] = (k[31] & 127u) | 64u;
}

static uint32_t s_boot_epoch = 1;
static uint32_t s_hs_seq     = 0;

static void ts_save_keys(void)
{
    vfs_mkdir("/home");
    int fd = vfs_open(TAILSCALE_CFG_PATH, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
    if (fd < 0) return;

    char mhex[65], nhex[65], dhex[65];
    bytes_to_hex(s_machine_priv, 32, mhex);
    bytes_to_hex(s_node_priv,    32, nhex);
    bytes_to_hex(s_disco_priv,   32, dhex);

    char buf[512];
    int n = ksnprintf(buf, sizeof(buf),
                      "machine=%s\nnode=%s\ndisco=%s\nip=%u.%u.%u.%u\nepoch=%u\nauthkey=%s\n",
                      mhex, nhex, dhex,
                      s_my_ip[0], s_my_ip[1], s_my_ip[2], s_my_ip[3],
                      (unsigned)s_boot_epoch,
                      s_saved_authkey);
    if (n > 0) vfs_write(fd, buf, (size_t)n);
    vfs_close(fd);
}

static void ts_load_or_gen_keys(void)
{
    if (s_keys_loaded) return;

    bool have_m = false, have_n = false, have_d = false;
    int fd = vfs_open(TAILSCALE_CFG_PATH, VFS_O_READ);
    if (fd >= 0) {
        char buf[512];
        int n = vfs_read(fd, buf, sizeof(buf) - 1);
        vfs_close(fd);
        if (n > 0) {
            buf[n] = 0;
            int pos = 0;
            while (pos < n) {
                char line[200];
                int l = 0;
                while (pos < n && buf[pos] != '\n' && buf[pos] != '\r') {
                    if (l < (int)sizeof(line) - 1) line[l++] = buf[pos];
                    pos++;
                }
                while (pos < n && (buf[pos] == '\n' || buf[pos] == '\r')) pos++;
                line[l] = 0;
                if (str_starts_with(line, "machine=") && str_len(line + 8) >= 64) {
                    have_m = hex_to_bytes(line + 8, s_machine_priv, 32);
                } else if (str_starts_with(line, "node=") && str_len(line + 5) >= 64) {
                    have_n = hex_to_bytes(line + 5, s_node_priv, 32);
                } else if (str_starts_with(line, "disco=") && str_len(line + 6) >= 64) {
                    have_d = hex_to_bytes(line + 6, s_disco_priv, 32);
                } else if (str_starts_with(line, "epoch=")) {
                    uint32_t ep = 0;
                    for (const char *ep_s = line + 6; *ep_s >= '0' && *ep_s <= '9'; ep_s++) {
                        ep = ep * 10u + (uint32_t)(*ep_s - '0');
                    }
                    s_boot_epoch = ep + 1u;
                } else if (str_starts_with(line, "authkey=")) {
                    str_copy(s_saved_authkey, sizeof(s_saved_authkey), line + 8);
                }
            }
        }
    }

    uint8_t hw_mac[6] = {0};
    esp_efuse_mac_get_default(hw_mac);
    if (!have_m) {
        blake2s_hash2(s_machine_priv, "MeOS-TS-MachineKey-v1", 21, hw_mac, 6);
        clamp_x25519_priv(s_machine_priv);
    }
    if (!have_n) {
        blake2s_hash2(s_node_priv, "MeOS-TS-NodeKey-v1", 18, hw_mac, 6);
        clamp_x25519_priv(s_node_priv);
    }
    if (!have_d) {
        blake2s_hash2(s_disco_priv, "MeOS-TS-DiscoKey-v1", 19, hw_mac, 6);
        clamp_x25519_priv(s_disco_priv);
    }

    x25519_base(s_machine_pub, s_machine_priv);
    x25519_base(s_node_pub,    s_node_priv);
    x25519_base(s_disco_pub,   s_disco_priv);
    s_keys_loaded = true;

    ts_save_keys();
}

/* =========================================================================
 * 6. WireGuard Data Plane (`Noise_IKpsk2_25519_ChaChaPoly_BLAKE2s`) + Disco
 * ========================================================================= */

/* Precomputed BLAKE2s("Noise_IKpsk2_25519_ChaChaPoly_BLAKE2s") and BLAKE2s(ck || "WireGuard v1 zx2c4 Jason@zx2c4.com") */
static const uint8_t s_wg_init_ck[32] = {
    0x60,0xe2,0x6d,0xae,0xf3,0x27,0xef,0xc0,0x2e,0xc3,0x35,0xe2,0xa0,0x25,0xd2,0xd0,
    0x16,0xeb,0x42,0x06,0xf8,0x72,0x77,0xf5,0x2d,0x38,0xd1,0x98,0x8b,0x78,0xcd,0x36
};
static const uint8_t s_wg_init_h0[32] = {
    0x22,0x11,0xb3,0x61,0x08,0x1a,0xc5,0x66,0x69,0x12,0x43,0xdb,0x45,0x8a,0xd5,0x32,
    0x2d,0x9c,0x6c,0x66,0x22,0x93,0xe8,0xb7,0x0e,0xe1,0x9c,0x65,0xba,0x07,0x9e,0xf3
};

static const uint8_t s_disco_magic[6] = { 0x54, 0x53, 0xf0, 0x9f, 0x92, 0xac };

static void disco_send_ping_locked(ts_peer_slot_t *p)
{
    if (!p || !p->info.valid || p->info.endpoint_ip[0] == 0 || p->info.endpoint_port == 0) return;

    /* Disco Ping payload: [0x01, 0x00, TxID(12B), NodeKey(32B)] = 46 bytes */
    uint8_t pt[46];
    pt[0] = 0x01u;
    pt[1] = 0x00u;
    rng_bytes(&pt[2], 12);
    if (s_ping_waiting && mem_eq(p->info.tailscale_ip, s_ping_target_ip, 4)) {
        mem_copy(s_ping_txid, &pt[2], 12);
    }
    mem_copy(&pt[14], s_node_pub, 32);

    uint8_t pkt[6 + 32 + 24 + 16 + 46];
    mem_copy(&pkt[0], s_disco_magic, 6);
    mem_copy(&pkt[6], s_disco_pub, 32);
    rng_bytes(&pkt[38], 24);
    xsalsa20_poly1305_seal(&pkt[62], p->disco_shared, &pkt[38], pt, sizeof(pt));

    p->last_disco_tx_us = hal_time_now_us();
    wifi_udp_send(p->info.endpoint_ip, TS_WG_PORT, p->info.endpoint_port, pkt, sizeof(pkt));
}

static void disco_send_pong(const uint8_t shared_key[32], const uint8_t txid[12],
                            const uint8_t src_ip[4], uint16_t src_port)
{
    /* Disco Pong payload: [0x02, 0x00, TxID(12B), IPv4MappedIPv6(16B), uint16_be(port)] = 32 bytes */
    uint8_t pt[32];
    mem_zero(pt, sizeof(pt));
    pt[0] = 0x02u;
    pt[1] = 0x00u;
    mem_copy(&pt[2], txid, 12);
    pt[24] = 0xFFu;
    pt[25] = 0xFFu;
    mem_copy(&pt[26], src_ip, 4);
    pt[30] = (uint8_t)(src_port >> 8);
    pt[31] = (uint8_t)(src_port & 0xFFu);

    uint8_t pkt[6 + 32 + 24 + 16 + 32];
    mem_copy(&pkt[0], s_disco_magic, 6);
    mem_copy(&pkt[6], s_disco_pub, 32);
    rng_bytes(&pkt[38], 24);
    xsalsa20_poly1305_seal(&pkt[62], shared_key, &pkt[38], pt, sizeof(pt));

    wifi_udp_send(src_ip, TS_WG_PORT, src_port, pkt, sizeof(pkt));
}

static bool wg_send_handshake_initiation_locked(ts_peer_slot_t *p)
{
    if (!p || !p->info.valid || p->info.endpoint_ip[0] == 0 || p->info.endpoint_port == 0) return false;

    /* Send a Disco Ping first so peer's magicsock associates our UDP endpoint with our DiscoKey/NodeKey */
    disco_send_ping_locked(p);

    uint8_t ck[32], h[32], k[32], e_pub[32], dh[32], zero_nonce[12];
    mem_zero(zero_nonce, 12);
    mem_copy(ck, s_wg_init_ck, 32);
    blake2s_hash2(h, s_wg_init_h0, 32, p->info.node_pub, 32);

    rng_bytes(p->hs_e_priv, 32);
    clamp_x25519_priv(p->hs_e_priv);
    x25519_base(e_pub, p->hs_e_priv);

    /* e */
    blake2s_hash2(h, h, 32, e_pub, 32);
    hkdf_blake2s_2(ck, NULL, ck, e_pub, 32);

    /* es */
    x25519_scalarmult(dh, p->hs_e_priv, p->info.node_pub);
    hkdf_blake2s_2(ck, k, ck, dh, 32);

    /* s */
    uint8_t enc_s[48];
    chachapoly_seal(enc_s, k, zero_nonce, s_node_pub, 32, h, 32);
    blake2s_hash2(h, h, 32, enc_s, 48);

    /* ss */
    x25519_scalarmult(dh, s_node_priv, p->info.node_pub);
    hkdf_blake2s_2(ck, k, ck, dh, 32);

    /* TAI64N timestamp (12 bytes: strictly monotonic across boots & handshakes) */
    uint8_t tai64n[12];
    s_hs_seq++;
    uint64_t secs = 0x4000000067400000ULL + ((uint64_t)s_boot_epoch * 4096ULL) +
                    (hal_time_now_us() / 1000000ULL) + (uint64_t)s_hs_seq;
    uint32_t nano = (uint32_t)((hal_time_now_us() % 1000000ULL) * 1000ULL);
    for (int i = 0; i < 8; i++) tai64n[i] = (uint8_t)((secs >> ((7 - i) * 8)) & 0xFFu);
    for (int i = 0; i < 4; i++) tai64n[8 + i] = (uint8_t)((nano >> ((3 - i) * 8)) & 0xFFu);

    uint8_t enc_ts[28];
    chachapoly_seal(enc_ts, k, zero_nonce, tai64n, 12, h, 32);
    blake2s_hash2(h, h, 32, enc_ts, 28);

    p->my_index = ++s_next_wg_index;
    mem_copy(p->hs_ck, ck, 32);
    mem_copy(p->hs_h,  h,  32);
    p->wg_hs_pending = true;
    p->last_hs_tx_us = hal_time_now_us();

    uint8_t pkt[148];
    mem_zero(pkt, sizeof(pkt));
    pkt[0] = 1u; /* Type 1: Handshake Initiation */
    put_le32(&pkt[4], p->my_index);
    mem_copy(&pkt[8],  e_pub,  32);
    mem_copy(&pkt[40], enc_s,  48);
    mem_copy(&pkt[88], enc_ts, 28);

    uint8_t mac1_key[32];
    blake2s_hash2(mac1_key, "mac1----", 8, p->info.node_pub, 32);
    blake2s_mac16(&pkt[116], mac1_key, pkt, 116);

    return wifi_udp_send(p->info.endpoint_ip, TS_WG_PORT, p->info.endpoint_port, pkt, sizeof(pkt));
}

static void wg_send_keepalive_locked(ts_peer_slot_t *p)
{
    if (!p || !p->info.wg_established) return;
    uint8_t pkt[32], nonce[12];
    mem_zero(pkt, 16);
    pkt[0] = 4u; /* Type 4: Transport Data */
    put_le32(&pkt[4], p->peer_index);
    uint64_t ctr = p->tx_counter++;
    put_le64(&pkt[8], ctr);
    wg_nonce_le(nonce, ctr);
    chachapoly_seal(&pkt[16], p->tx_key, nonce, NULL, 0, NULL, 0);
    p->info.tx_packets++;
    wifi_udp_send(p->info.endpoint_ip, TS_WG_PORT, p->info.endpoint_port, pkt, 32);
}

static bool wg_send_ipv4_to_peer_locked(ts_peer_slot_t *p, const uint8_t *ipv4_pkt, size_t len)
{
    if (!p || !p->info.valid || len == 0 || len > 1400u) return false;

    if (!p->info.wg_established) {
        uint64_t now = hal_time_now_us();
        if (!p->wg_hs_pending || (now - p->last_hs_tx_us > 1500000ULL)) {
            wg_send_handshake_initiation_locked(p);
        }
        return false;
    }

    size_t padded_len = (len + 15u) & ~(size_t)15u;
    static uint8_t pt[1408];
    mem_copy(pt, ipv4_pkt, len);
    if (padded_len > len) mem_zero(pt + len, padded_len - len);

    static uint8_t pkt[16 + 1408 + 16];
    uint8_t nonce[12];
    mem_zero(pkt, 16);
    pkt[0] = 4u;
    put_le32(&pkt[4], p->peer_index);
    uint64_t ctr = p->tx_counter++;
    put_le64(&pkt[8], ctr);
    wg_nonce_le(nonce, ctr);
    chachapoly_seal(&pkt[16], p->tx_key, nonce, pt, padded_len, NULL, 0);

    p->info.tx_packets++;
    return wifi_udp_send(p->info.endpoint_ip, TS_WG_PORT, p->info.endpoint_port, pkt, 16u + padded_len + 16u);
}

static ts_peer_slot_t *find_peer_by_ip_locked(const uint8_t ip[4])
{
    for (int i = 0; i < s_peer_count; i++) {
        if (s_peers[i].info.valid && mem_eq(s_peers[i].info.tailscale_ip, ip, 4)) {
            return &s_peers[i];
        }
    }
    return NULL;
}

/* Virtual tunnel TX callback invoked by wifi_tab5.c for TCP/ICMP packets to 100.64.0.0/10 */
static bool ts_tun_tx_callback(const uint8_t *ipv4_pkt, size_t len)
{
    if (!ipv4_pkt || len < 20u || len > 1480u) return false;
    ts_ensure_lock();
    ts_lock();
    ts_peer_slot_t *p = find_peer_by_ip_locked(&ipv4_pkt[16]);
    if (!p) {
        ts_unlock();
        return false;
    }

    static uint8_t s_pending_tun_tx[1480];
    const uint8_t *pkt_to_send = ipv4_pkt;

    uint64_t now_us = hal_time_now_us();
    if (p->info.wg_established && p->last_rx_us > 0 && (now_us - p->last_rx_us > 20000000ULL)) {
        p->info.wg_established = false;
    }

    /* If WireGuard session isn't established yet, initiate handshake and poll SDIO RX while waiting */
    if (!p->info.wg_established) {
        mem_copy(s_pending_tun_tx, ipv4_pkt, len);
        pkt_to_send = s_pending_tun_tx;
        disco_send_ping_locked(p);
        for (int w = 0; w < 6; w++) {
            uint32_t saved_d = ts_unlock_all();
            wifi_status_t wst;
            wifi_get_status(&wst);
            task_sleep(10);
            ts_relock_all(saved_d);
        }
        wg_send_handshake_initiation_locked(p);
        uint64_t deadline = hal_time_now_us() + 3500000ULL;
        while (hal_time_now_us() < deadline && !p->info.wg_established) {
            uint32_t saved_d = ts_unlock_all();
            wifi_status_t wst;
            wifi_get_status(&wst); /* Polls SDIO RX so WG Type-2 Handshake Response arrives promptly */
            task_sleep(10);
            ts_relock_all(saved_d);
            if (!p->info.wg_established && (hal_time_now_us() - p->last_hs_tx_us >= 1200000ULL)) {
                disco_send_ping_locked(p);
                wg_send_handshake_initiation_locked(p);
            }
        }
    } else if (now_us - p->last_disco_tx_us >= 8000000ULL) {
        disco_send_ping_locked(p);
    }
    bool ok = wg_send_ipv4_to_peer_locked(p, pkt_to_send, len);
    ts_unlock();
    return ok;
}

/* Virtual MagicDNS hostname resolver invoked by wifi_resolve_host() */
static bool ts_tun_resolve_callback(const char *host, uint8_t out_ip[4])
{
    if (!host || !host[0] || !out_ip) return false;
    /* Strip optional .ts.net suffix or trailing dot */
    char short_name[48];
    size_t i = 0;
    while (host[i] && host[i] != '.' && i < sizeof(short_name) - 1) {
        short_name[i] = host[i];
        i++;
    }
    short_name[i] = 0;
    size_t slen = i;

    ts_ensure_lock();
    ts_lock();
    if (s_peer_count == 0 && s_state != TS_STATE_RUNNING) {
        uint32_t saved_d = ts_unlock_all();
        tailscale_up(NULL);
        ts_relock_all(saved_d);
    }

    if (s_my_ip[0] != 0 && (str_ieq(host, s_hostname) || str_ieq(short_name, s_hostname))) {
        mem_copy(out_ip, s_my_ip, 4);
        ts_unlock();
        return true;
    }

    /* 1st pass: exact case-insensitive match on primary MagicDNS hostname or OS Hostname */
    for (int p = 0; p < s_peer_count; p++) {
        if (!s_peers[p].info.valid) continue;
        if (str_ieq(host, s_peers[p].info.hostname) ||
            str_ieq(short_name, s_peers[p].info.hostname) ||
            (s_peers[p].alt_hostname[0] &&
             (str_ieq(host, s_peers[p].alt_hostname) || str_ieq(short_name, s_peers[p].alt_hostname)))) {
            mem_copy(out_ip, s_peers[p].info.tailscale_ip, 4);
            ts_unlock();
            return true;
        }
    }

    /* 2nd pass: prefix match for suffixed Tailscale names (e.g. "meos-tab5" matching "meos-tab5-1") */
    for (int p = 0; p < s_peer_count; p++) {
        if (!s_peers[p].info.valid) continue;
        const char *hn = s_peers[p].info.hostname;
        size_t hlen = str_len(hn);
        if (hlen > slen + 1 && hn[slen] == '-') {
            char prefix[48];
            mem_copy(prefix, hn, slen);
            prefix[slen] = 0;
            if (str_ieq(short_name, prefix)) {
                mem_copy(out_ip, s_peers[p].info.tailscale_ip, 4);
                ts_unlock();
                return true;
            }
        }
    }

    ts_unlock();
    return false;
}

/* UDP RX callback registered on port 41641 for incoming Disco & WireGuard packets */
static void ts_udp_rx_callback(const uint8_t src_ip[4], uint16_t src_port,
                               uint16_t dst_port, const uint8_t *data, size_t len)
{
    (void)dst_port;
    if (!data || len < 4u) return;

    ts_ensure_lock();
    ts_lock();

    /* 1. Check for Tailscale Disco packet ("TS\xf0\x9f\x92\xac") */
    if (len >= 6u + 32u + 24u + 16u && mem_eq(data, s_disco_magic, 6)) {
        const uint8_t *sender_disco = &data[6];
        const uint8_t *nonce24      = &data[38];
        const uint8_t *box          = &data[62];
        size_t box_len              = len - 62u;

        ts_peer_slot_t *peer = NULL;
        for (int i = 0; i < s_peer_count; i++) {
            if (s_peers[i].info.valid && mem_eq(s_peers[i].info.disco_pub, sender_disco, 32)) {
                peer = &s_peers[i];
                break;
            }
        }
        uint8_t shared[32];
        if (peer) {
            mem_copy(shared, peer->disco_shared, 32);
        } else {
            nacl_box_shared_key(shared, s_disco_priv, sender_disco);
        }

        uint8_t pt[128];
        if (box_len <= sizeof(pt) + 16u && xsalsa20_poly1305_open(pt, shared, nonce24, box, box_len)) {
            size_t pt_len = box_len - 16u;
            if (pt_len >= 14u && pt[0] == 0x01u) {
                /* Disco Ping -> update endpoint and send Disco Pong */
                if (peer) {
                    mem_copy(peer->info.endpoint_ip, src_ip, 4);
                    peer->info.endpoint_port = src_port;
                    peer->info.online = true;
                }
                disco_send_pong(shared, &pt[2], src_ip, src_port);
                if (peer && peer->wg_hs_pending && !peer->info.wg_established) {
                    uint64_t now = hal_time_now_us();
                    if (now > 1100000ULL) {
                        peer->last_hs_tx_us = now - 1100000ULL;
                    }
                }
            } else if (pt_len >= 14u && pt[0] == 0x02u) {
                /* Disco Pong */
                if (peer) {
                    mem_copy(peer->info.endpoint_ip, src_ip, 4);
                    peer->info.endpoint_port = src_port;
                    peer->info.online = true;
                    uint64_t now = hal_time_now_us();
                    if (peer->last_disco_tx_us > 0 && now >= peer->last_disco_tx_us) {
                        uint32_t rtt = (uint32_t)((now - peer->last_disco_tx_us) / 1000ULL);
                        if (rtt == 0) rtt = 1;
                        peer->info.last_rtt_ms = rtt;
                    }
                }
                if (s_ping_waiting && mem_eq(&pt[2], s_ping_txid, 12)) {
                    uint64_t rtt = (hal_time_now_us() - s_ping_tx_us) / 1000ULL;
                    s_ping_rtt_ms = (rtt == 0) ? 1u : (uint32_t)rtt;
                    mem_copy(s_ping_pong_ip, src_ip, 4);
                    s_ping_pong_port = src_port;
                    s_ping_done = true;
                }
            }
        }
        ts_unlock();
        return;
    }

    uint8_t msg_type = data[0];

    /* 2. WireGuard Type 1: Incoming Handshake Initiation (148 bytes) */
    if (msg_type == 1u && len >= 148u) {
        uint32_t sender_idx = get_le32(&data[4]);
        const uint8_t *re_pub = &data[8];
        const uint8_t *enc_s  = &data[40];
        const uint8_t *enc_ts = &data[88];

        uint8_t ck[32], h[32], k[32], dh[32], peer_s[32], ts_buf[12], zero_nonce[12];
        mem_zero(zero_nonce, 12);
        mem_copy(ck, s_wg_init_ck, 32);
        blake2s_hash2(h, s_wg_init_h0, 32, s_node_pub, 32);
        blake2s_hash2(h, h, 32, re_pub, 32);
        hkdf_blake2s_2(ck, NULL, ck, re_pub, 32);

        x25519_scalarmult(dh, s_node_priv, re_pub);
        hkdf_blake2s_2(ck, k, ck, dh, 32);
        if (!chachapoly_open(peer_s, k, zero_nonce, enc_s, 48, h, 32)) {
            ts_unlock();
            return;
        }
        blake2s_hash2(h, h, 32, enc_s, 48);

        x25519_scalarmult(dh, s_node_priv, peer_s);
        hkdf_blake2s_2(ck, k, ck, dh, 32);
        if (!chachapoly_open(ts_buf, k, zero_nonce, enc_ts, 28, h, 32)) {
            ts_unlock();
            return;
        }
        blake2s_hash2(h, h, 32, enc_ts, 28);

        ts_peer_slot_t *peer = NULL;
        for (int i = 0; i < s_peer_count; i++) {
            if (s_peers[i].info.valid && mem_eq(s_peers[i].info.node_pub, peer_s, 32)) {
                peer = &s_peers[i];
                break;
            }
        }
        if (!peer) {
            ts_unlock();
            return;
        }

        /* Build Type 2 Handshake Response (92 bytes) */
        uint8_t e_priv[32], e_pub[32];
        rng_bytes(e_priv, 32);
        clamp_x25519_priv(e_priv);
        x25519_base(e_pub, e_priv);

        blake2s_hash2(h, h, 32, e_pub, 32);
        hkdf_blake2s_2(ck, NULL, ck, e_pub, 32);

        /* ee */
        x25519_scalarmult(dh, e_priv, re_pub);
        hkdf_blake2s_2(ck, NULL, ck, dh, 32);

        /* se */
        x25519_scalarmult(dh, e_priv, peer_s);
        hkdf_blake2s_2(ck, NULL, ck, dh, 32);

        /* psk (zero32) */
        uint8_t zero32[32], tau[32], empty_tag[16];
        mem_zero(zero32, 32);
        hkdf_blake2s_3(ck, tau, k, ck, zero32, 32);
        blake2s_hash2(h, h, 32, tau, 32);
        chachapoly_seal(empty_tag, k, zero_nonce, NULL, 0, h, 32);
        blake2s_hash2(h, h, 32, empty_tag, 16);

        /* Responder keys: rx_key = T1, tx_key = T2 */
        hkdf_blake2s_2(peer->rx_key, peer->tx_key, ck, NULL, 0);
        peer->my_index   = ++s_next_wg_index;
        peer->peer_index = sender_idx;
        peer->tx_counter = 0;
        peer->wg_hs_pending = false;
        peer->info.wg_established = true;
        peer->info.online = true;
        peer->last_rx_us  = hal_time_now_us();
        mem_copy(peer->info.endpoint_ip, src_ip, 4);
        peer->info.endpoint_port = src_port;

        uint8_t resp[92];
        mem_zero(resp, sizeof(resp));
        resp[0] = 2u;
        put_le32(&resp[4], peer->my_index);
        put_le32(&resp[8], sender_idx);
        mem_copy(&resp[12], e_pub, 32);
        mem_copy(&resp[44], empty_tag, 16);

        uint8_t mac1_key[32];
        blake2s_hash2(mac1_key, "mac1----", 8, peer_s, 32);
        blake2s_mac16(&resp[60], mac1_key, resp, 60);
        wifi_udp_send(src_ip, TS_WG_PORT, src_port, resp, sizeof(resp));
        ts_unlock();
        return;
    }

    /* 3. WireGuard Type 2: Handshake Response (92 bytes) */
    if (msg_type == 2u && len >= 92u) {
        uint32_t sender_idx   = get_le32(&data[4]);
        uint32_t receiver_idx = get_le32(&data[8]);
        const uint8_t *re_pub = &data[12];
        const uint8_t *enc_empty = &data[44];

        ts_peer_slot_t *peer = NULL;
        for (int i = 0; i < s_peer_count; i++) {
            if (s_peers[i].info.valid && s_peers[i].wg_hs_pending && s_peers[i].my_index == receiver_idx) {
                peer = &s_peers[i];
                break;
            }
        }
        if (!peer) {
            ts_unlock();
            return;
        }

        uint8_t ck[32], h[32], dh[32], tau[32], k[32], zero32[32], zero_nonce[12];
        mem_zero(zero32, 32);
        mem_zero(zero_nonce, 12);
        mem_copy(ck, peer->hs_ck, 32);
        mem_copy(h,  peer->hs_h,  32);

        /* e */
        blake2s_hash2(h, h, 32, re_pub, 32);
        hkdf_blake2s_2(ck, NULL, ck, re_pub, 32);

        /* ee */
        x25519_scalarmult(dh, peer->hs_e_priv, re_pub);
        hkdf_blake2s_2(ck, NULL, ck, dh, 32);

        /* se */
        x25519_scalarmult(dh, s_node_priv, re_pub);
        hkdf_blake2s_2(ck, NULL, ck, dh, 32);

        /* psk */
        hkdf_blake2s_3(ck, tau, k, ck, zero32, 32);
        blake2s_hash2(h, h, 32, tau, 32);

        if (!chachapoly_open(NULL, k, zero_nonce, enc_empty, 16, h, 32)) {
            ts_unlock();
            return;
        }

        /* Initiator transport keys: tx_key = T1, rx_key = T2 */
        hkdf_blake2s_2(peer->tx_key, peer->rx_key, ck, NULL, 0);
        peer->peer_index          = sender_idx;
        peer->tx_counter          = 0;
        peer->wg_hs_pending       = false;
        peer->info.wg_established = true;
        peer->info.online         = true;
        peer->last_rx_us          = hal_time_now_us();
        mem_copy(peer->info.endpoint_ip, src_ip, 4);
        peer->info.endpoint_port  = src_port;

        /* Confirm session with an immediate empty Type-4 keepalive */
        wg_send_keepalive_locked(peer);
        ts_unlock();
        return;
    }

    /* 4. WireGuard Type 4: Transport Data (header 16B + ciphertext + 16B tag) */
    if (msg_type == 4u && len >= 32u) {
        uint32_t receiver_idx = get_le32(&data[4]);
        uint64_t counter      = (uint64_t)get_le32(&data[8]) | ((uint64_t)get_le32(&data[12]) << 32);

        ts_peer_slot_t *peer = NULL;
        for (int i = 0; i < s_peer_count; i++) {
            if (s_peers[i].info.valid && s_peers[i].info.wg_established && s_peers[i].my_index == receiver_idx) {
                peer = &s_peers[i];
                break;
            }
        }
        if (!peer) {
            ts_unlock();
            return;
        }

        size_t ct_and_tag_len = len - 16u;
        if (ct_and_tag_len > 1500u) {
            ts_unlock();
            return;
        }

        static uint8_t s_wg_pt[1500];
        uint8_t nonce[12];
        wg_nonce_le(nonce, counter);
        if (!chachapoly_open(s_wg_pt, peer->rx_key, nonce, &data[16], ct_and_tag_len, NULL, 0)) {
            ts_unlock();
            return;
        }

        peer->info.rx_packets++;
        peer->info.online = true;
        peer->last_rx_us  = hal_time_now_us();
        size_t pt_len = ct_and_tag_len - 16u;

        /* Check if inner packet is IPv4 */
        if (pt_len >= 20u && (s_wg_pt[0] >> 4) == 4u) {
            size_t ip_len = ((size_t)s_wg_pt[2] << 8) | s_wg_pt[3];
            if (ip_len >= 20u && ip_len <= pt_len) {
                /* Check for ICMP Echo Reply to our tailscale ping */
                size_t ihl = (size_t)(s_wg_pt[0] & 0x0Fu) * 4u;
                if (s_wg_pt[9] == 1u && ip_len >= ihl + 8u && s_wg_pt[ihl] == 0u) {
                    if (s_ping_waiting && mem_eq(&s_wg_pt[12], s_ping_target_ip, 4)) {
                        uint64_t rtt = (hal_time_now_us() - s_ping_tx_us) / 1000ULL;
                        s_ping_rtt_ms = (rtt == 0) ? 1u : (uint32_t)rtt;
                        mem_copy(s_ping_pong_ip, src_ip, 4);
                        s_ping_pong_port = src_port;
                        s_ping_done = true;
                    }
                }
                static uint8_t ip_copy[1480];
                size_t copy_len = ip_len > sizeof(ip_copy) ? sizeof(ip_copy) : ip_len;
                mem_copy(ip_copy, s_wg_pt, copy_len);
                uint32_t saved_d = ts_unlock_all();
                wifi_tun_inject_rx_ipv4(ip_copy, copy_len);
                if (saved_d > 1u) {
                    ts_relock_all(saved_d - 1u);
                }
                return;
            }
        }
        ts_unlock();
        return;
    }

    ts_unlock();
}

/* =========================================================================
 * 7. Tailscale ts2021 Control Client (Noise IK + EarlyPayload + HTTP/2)
 * ========================================================================= */

typedef struct {
    int      sock;
    uint8_t  tx_key[32];
    uint8_t  rx_key[32];
    uint64_t tx_nonce;
    uint64_t rx_nonce;
    uint8_t  pre_rx[256];
    size_t   pre_len;
    size_t   pre_pos;
    uint8_t  plain_buf[4096];
    size_t   plain_len;
    size_t   plain_pos;
} ts_ctrl_conn_t;

static bool ctrl_raw_recv_exact(ts_ctrl_conn_t *c, uint8_t *out, size_t n, uint32_t timeout_ms)
{
    size_t got = 0;
    while (got < n && c->pre_pos < c->pre_len) {
        out[got++] = c->pre_rx[c->pre_pos++];
    }
    uint64_t deadline = hal_time_now_us() + (uint64_t)timeout_ms * 1000ULL;
    while (got < n && hal_time_now_us() < deadline) {
        int r = wifi_tcp_recv_sock(c->sock, out + got, n - got, 250);
        if (r < 0) return false;
        got += (size_t)r;
    }
    return got == n;
}

static bool ctrl_noise_write(ts_ctrl_conn_t *c, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    static uint8_t s_frame_buf[2060];
    while (len > 0) {
        size_t chunk = (len > 2000u) ? 2000u : len;
        size_t ct_len = chunk + 16u;
        s_frame_buf[0] = 0x04u;
        s_frame_buf[1] = (uint8_t)(ct_len >> 8);
        s_frame_buf[2] = (uint8_t)(ct_len & 0xFFu);

        uint8_t nonce[12];
        noise_nonce_be(nonce, c->tx_nonce++);
        chachapoly_seal(&s_frame_buf[3], c->tx_key, nonce, p, chunk, NULL, 0);

        if (wifi_tcp_send_sock(c->sock, s_frame_buf, 3u + ct_len, 5000) != (int)(3u + ct_len)) {
            return false;
        }
        p   += chunk;
        len -= chunk;
    }
    return true;
}

static bool ctrl_noise_read_exact(ts_ctrl_conn_t *c, uint8_t *out, size_t want, uint32_t timeout_ms)
{
    size_t got = 0;
    static uint8_t s_ct_buf[4112];

    while (got < want) {
        while (got < want && c->plain_pos < c->plain_len) {
            out[got++] = c->plain_buf[c->plain_pos++];
        }
        if (got >= want) break;

        uint8_t hdr[3];
        if (!ctrl_raw_recv_exact(c, hdr, 3, timeout_ms)) return false;
        uint16_t ct_len = ((uint16_t)hdr[1] << 8) | hdr[2];
        if (hdr[0] != 0x04u || ct_len < 16u || ct_len > sizeof(s_ct_buf)) return false;

        if (!ctrl_raw_recv_exact(c, s_ct_buf, ct_len, timeout_ms)) return false;
        uint8_t nonce[12];
        noise_nonce_be(nonce, c->rx_nonce++);
        if (!chachapoly_open(c->plain_buf, c->rx_key, nonce, s_ct_buf, ct_len, NULL, 0)) {
            return false;
        }
        c->plain_len = (size_t)ct_len - 16u;
        c->plain_pos = 0;
    }
    return true;
}

static bool ctrl_connect_and_handshake(ts_ctrl_conn_t *c)
{
    mem_zero(c, sizeof(*c));
    c->sock = TS_SOCK_CTRL;

    uint8_t ctrl_ip[4];
    if (!wifi_resolve_host("controlplane.tailscale.com", ctrl_ip)) {
        ts_set_msg("DNS resolve controlplane.tailscale.com failed");
        return false;
    }

    if (!wifi_tcp_connect_sock(c->sock, ctrl_ip, 80, 5000)) {
        ts_set_msg("TCP connect controlplane.tailscale.com:80 failed");
        return false;
    }

    /* Noise_IK_25519_ChaChaPoly_BLAKE2s (Protocol Version = 109) */
    uint8_t ck[32], h[32], k[32], dh[32], e_priv[32], e_pub[32], zero_nonce[12];
    mem_zero(zero_nonce, 12);
    blake2s_hash(ck, "Noise_IK_25519_ChaChaPoly_BLAKE2s", 33);
    blake2s_hash2(h, ck, 32, "Tailscale Control Protocol v109", 31);
    blake2s_hash2(h, h, 32, s_control_pub, 32);

    rng_bytes(e_priv, 32);
    clamp_x25519_priv(e_priv);
    x25519_base(e_pub, e_priv);

    /* e */
    blake2s_hash2(h, h, 32, e_pub, 32);
    /* es */
    x25519_scalarmult(dh, e_priv, s_control_pub);
    hkdf_blake2s_2(ck, k, ck, dh, 32);
    /* s */
    uint8_t enc_s[48];
    chachapoly_seal(enc_s, k, zero_nonce, s_machine_pub, 32, h, 32);
    blake2s_hash2(h, h, 32, enc_s, 48);
    /* ss */
    x25519_scalarmult(dh, s_machine_priv, s_control_pub);
    hkdf_blake2s_2(ck, k, ck, dh, 32);
    /* empty payload tag */
    uint8_t init_tag[16];
    chachapoly_seal(init_tag, k, zero_nonce, NULL, 0, h, 32);
    blake2s_hash2(h, h, 32, init_tag, 16);

    uint8_t init_msg[101];
    init_msg[0] = 0x00u; init_msg[1] = 0x6du; /* version = 109 */
    init_msg[2] = 0x01u; init_msg[3] = 0x00u; init_msg[4] = 0x60u; /* type 1, len 96 */
    mem_copy(&init_msg[5],  e_pub,    32);
    mem_copy(&init_msg[37], enc_s,    48);
    mem_copy(&init_msg[85], init_tag, 16);

    char b64_init[144];
    base64_encode(init_msg, sizeof(init_msg), b64_init);

    char req[360];
    int req_len = ksnprintf(req, sizeof(req),
                            "GET /ts2021 HTTP/1.1\r\n"
                            "Host: controlplane.tailscale.com\r\n"
                            "Upgrade: tailscale-control-protocol\r\n"
                            "Connection: Upgrade\r\n"
                            "X-Tailscale-Handshake: %s\r\n\r\n",
                            b64_init);
    if (wifi_tcp_send_sock(c->sock, req, (size_t)req_len, 4000) != req_len) {
        wifi_tcp_close_sock(c->sock);
        ts_set_msg("Failed to send HTTP Upgrade request");
        return false;
    }

    /* Read HTTP 101 Switching Protocols header up to \r\n\r\n */
    uint8_t hdr_buf[512];
    size_t hdr_len = 0;
    int body_start = -1;
    uint64_t deadline = hal_time_now_us() + 5000000ULL;
    while (hal_time_now_us() < deadline && hdr_len < sizeof(hdr_buf)) {
        int r = wifi_tcp_recv_sock(c->sock, hdr_buf + hdr_len, sizeof(hdr_buf) - hdr_len, 300);
        if (r < 0) break;
        hdr_len += (size_t)r;
        for (size_t i = 0; i + 3 < hdr_len; i++) {
            if (hdr_buf[i] == '\r' && hdr_buf[i + 1] == '\n' &&
                hdr_buf[i + 2] == '\r' && hdr_buf[i + 3] == '\n') {
                body_start = (int)(i + 4);
                break;
            }
        }
        if (body_start >= 0) break;
    }
    if (body_start < 0) {
        wifi_tcp_close_sock(c->sock);
        ts_set_msg("Did not receive HTTP 101 Switching Protocols");
        return false;
    }

    size_t rem = hdr_len - (size_t)body_start;
    if (rem > sizeof(c->pre_rx)) rem = sizeof(c->pre_rx);
    mem_copy(c->pre_rx, hdr_buf + body_start, rem);
    c->pre_len = rem;
    c->pre_pos = 0;

    /* Read 51-byte Noise responseMessage: [0x02, 0x00, 0x30] + re_pub(32B) + tag(16B) */
    uint8_t resp_msg[51];
    if (!ctrl_raw_recv_exact(c, resp_msg, 51, 5000) ||
        resp_msg[0] != 0x02u || resp_msg[1] != 0x00u || resp_msg[2] != 0x30u) {
        wifi_tcp_close_sock(c->sock);
        ts_set_msg("Invalid Noise handshake response header");
        return false;
    }

    const uint8_t *re_pub   = &resp_msg[3];
    const uint8_t *resp_tag = &resp_msg[35];

    /* e */
    blake2s_hash2(h, h, 32, re_pub, 32);
    /* ee */
    x25519_scalarmult(dh, e_priv, re_pub);
    hkdf_blake2s_2(ck, NULL, ck, dh, 32);
    /* se */
    x25519_scalarmult(dh, s_machine_priv, re_pub);
    hkdf_blake2s_2(ck, k, ck, dh, 32);

    if (!chachapoly_open(NULL, k, zero_nonce, resp_tag, 16, h, 32)) {
        wifi_tcp_close_sock(c->sock);
        ts_set_msg("Noise handshake tag verification failed");
        return false;
    }

    /* Split chaining key into transport keys: tx_key = T1, rx_key = T2 */
    hkdf_blake2s_2(c->tx_key, c->rx_key, ck, NULL, 0);
    c->tx_nonce = 0;
    c->rx_nonce = 0;

    /* Send HTTP/2 client connection preface + SETTINGS (4MB initial window) + connection WINDOW_UPDATE (4MB) */
    static const uint8_t h2_preface[52] =
        "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"
        "\x00\x00\x06\x04\x00\x00\x00\x00\x00\x00\x04\x00\x40\x00\x00"
        "\x00\x00\x04\x08\x00\x00\x00\x00\x00\x00\x40\x00\x00";
    if (!ctrl_noise_write(c, h2_preface, sizeof(h2_preface))) {
        wifi_tcp_close_sock(c->sock);
        ts_set_msg("Failed to send HTTP/2 preface");
        return false;
    }

    /*
     * Consume EarlyPayloadMagic ("\xff\xff\xffTS" + 4-byte BE length + JSON challenge)
     * that controlplane.tailscale.com sends before the first HTTP/2 SETTINGS frame!
     */
    uint8_t early_hdr[9];
    if (!ctrl_noise_read_exact(c, early_hdr, 9, 5000)) {
        wifi_tcp_close_sock(c->sock);
        ts_set_msg("Failed to read control stream after handshake");
        return false;
    }
    if (early_hdr[0] == 0xFFu && early_hdr[1] == 0xFFu && early_hdr[2] == 0xFFu &&
        early_hdr[3] == 'T'   && early_hdr[4] == 'S') {
        uint32_t elen = ((uint32_t)early_hdr[5] << 24) | ((uint32_t)early_hdr[6] << 16) |
                        ((uint32_t)early_hdr[7] << 8)  | (uint32_t)early_hdr[8];
        uint8_t discard[128];
        while (elen > 0) {
            size_t take = (elen > sizeof(discard)) ? sizeof(discard) : elen;
            if (!ctrl_noise_read_exact(c, discard, take, 5000)) {
                wifi_tcp_close_sock(c->sock);
                return false;
            }
            elen -= (uint32_t)take;
        }
    }

    return true;
}

/* Encode literal HPACK header without indexing (0x00) */
static size_t hpack_add_literal(uint8_t *dst, const char *name, const char *val)
{
    size_t nlen = str_len(name);
    size_t vlen = str_len(val);
    size_t p = 0;
    dst[p++] = 0x00u;
    dst[p++] = (uint8_t)nlen;
    mem_copy(&dst[p], name, nlen); p += nlen;
    dst[p++] = (uint8_t)vlen;
    mem_copy(&dst[p], val, vlen);  p += vlen;
    return p;
}

static bool ctrl_h2_post(ts_ctrl_conn_t *c, uint32_t stream_id, const char *path,
                         const char *json_body, uint8_t *out_resp, size_t max_resp,
                         size_t *out_len, bool stop_after_first_data)
{
    size_t body_len = str_len(json_body);

    /* Build HTTP/2 HEADERS payload:
     *  0x83 (:method: POST), 0x86 (:scheme: http),
     *  literal :authority = controlplane.tailscale.com, literal :path = <path>
     */
    uint8_t hblock[160];
    size_t hlen = 0;
    hblock[hlen++] = 0x83u; /* :method: POST */
    hblock[hlen++] = 0x86u; /* :scheme: http */
    hlen += hpack_add_literal(&hblock[hlen], ":authority", "controlplane.tailscale.com");
    hlen += hpack_add_literal(&hblock[hlen], ":path", path);

    uint8_t hframe[172];
    hframe[0] = 0u;
    hframe[1] = (uint8_t)(hlen >> 8);
    hframe[2] = (uint8_t)(hlen & 0xFFu);
    hframe[3] = 0x01u; /* HEADERS */
    hframe[4] = 0x04u; /* END_HEADERS */
    hframe[5] = (uint8_t)(stream_id >> 24);
    hframe[6] = (uint8_t)(stream_id >> 16);
    hframe[7] = (uint8_t)(stream_id >> 8);
    hframe[8] = (uint8_t)(stream_id & 0xFFu);
    mem_copy(&hframe[9], hblock, hlen);

    if (!ctrl_noise_write(c, hframe, 9u + hlen)) return false;

    /* Send HTTP/2 DATA frame with END_STREAM (0x01) */
    uint8_t dhdr[9];
    dhdr[0] = 0u;
    dhdr[1] = (uint8_t)(body_len >> 8);
    dhdr[2] = (uint8_t)(body_len & 0xFFu);
    dhdr[3] = 0x00u; /* DATA */
    dhdr[4] = 0x01u; /* END_STREAM */
    dhdr[5] = (uint8_t)(stream_id >> 24);
    dhdr[6] = (uint8_t)(stream_id >> 16);
    dhdr[7] = (uint8_t)(stream_id >> 8);
    dhdr[8] = (uint8_t)(stream_id & 0xFFu);

    if (!ctrl_noise_write(c, dhdr, 9) || !ctrl_noise_write(c, json_body, body_len)) {
        return false;
    }

    /* Read HTTP/2 frames until stream ends or MapResponse message is complete */
    size_t total_data = 0;
    static uint8_t s_h2_scratch[2048];

    for (int iter = 0; iter < 1000; iter++) {
        uint8_t fhdr[9];
        if (!ctrl_noise_read_exact(c, fhdr, 9, 8000)) break;

        uint32_t flen  = ((uint32_t)fhdr[0] << 16) | ((uint32_t)fhdr[1] << 8) | fhdr[2];
        uint8_t  ftype = fhdr[3];
        uint8_t  fflags= fhdr[4];
        uint32_t fsid  = (((uint32_t)fhdr[5] & 0x7Fu) << 24) | ((uint32_t)fhdr[6] << 16) |
                         ((uint32_t)fhdr[7] << 8) | fhdr[8];

        if (ftype == 0x00u && fsid == stream_id) {
            /* DATA frame for our stream */
            uint32_t rem = flen;
            while (rem > 0) {
                size_t take = (rem > sizeof(s_h2_scratch)) ? sizeof(s_h2_scratch) : rem;
                if (!ctrl_noise_read_exact(c, s_h2_scratch, take, 8000)) return false;
                if (total_data < max_resp) {
                    size_t copy = (total_data + take > max_resp) ? (max_resp - total_data) : take;
                    mem_copy(out_resp + total_data, s_h2_scratch, copy);
                }
                total_data += take;
                rem -= (uint32_t)take;
            }
            /* Send WINDOW_UPDATE for BOTH connection (0) AND stream (`stream_id`) so large MapResponses never stall */
            if (flen > 0) {
                uint8_t wupd[26] = {
                    0,0,4, 8,0, 0,0,0,0, 0,1,0,0,
                    0,0,4, 8,0,
                    (uint8_t)(stream_id >> 24), (uint8_t)(stream_id >> 16),
                    (uint8_t)(stream_id >> 8),  (uint8_t)(stream_id & 0xFFu),
                    0,1,0,0
                };
                ctrl_noise_write(c, wupd, sizeof(wupd));
            }
            if (stop_after_first_data && total_data >= 4u) {
                uint32_t msg_len = get_le32(out_resp);
                if (total_data >= 4u + msg_len) break;
            }
            if (fflags & 0x01u) break; /* END_STREAM */
        } else {
            /* Drain non-DATA frame payload */
            uint32_t rem = flen;
            while (rem > 0) {
                size_t take = (rem > sizeof(s_h2_scratch)) ? sizeof(s_h2_scratch) : rem;
                if (!ctrl_noise_read_exact(c, s_h2_scratch, take, 8000)) return false;
                rem -= (uint32_t)take;
            }
            if (ftype == 0x04u && !(fflags & 0x01u)) {
                /* Reply with SETTINGS ACK */
                uint8_t sack[9] = {0,0,0, 4,1, 0,0,0,0};
                ctrl_noise_write(c, sack, 9);
            } else if (ftype == 0x06u && !(fflags & 0x01u) && flen == 8u) {
                /* Reply with PING ACK */
                uint8_t pack[17] = {0,0,8, 6,1, 0,0,0,0};
                mem_copy(&pack[9], s_h2_scratch, 8);
                ctrl_noise_write(c, pack, 17);
            } else if (ftype == 0x03u && fsid == stream_id) {
                /* RST_STREAM */
                break;
            }
        }
    }

    if (out_len) *out_len = total_data;
    return total_data > 0;
}

/* =========================================================================
 * 8. JSON Extractors for RegisterResponse and MapResponse
 * ========================================================================= */

static const char *json_find_range(const char *start, const char *end, const char *needle)
{
    if (!start || !end || !needle || start >= end) return NULL;
    size_t nlen = str_len(needle);
    if (nlen == 0) return start;
    for (const char *p = start; p + nlen <= end; p++) {
        if (mem_eq(p, needle, nlen)) return p;
    }
    return NULL;
}

static bool json_extract_string(const char *json, const char *end, const char *key, char *out, size_t max_out)
{
    if (!json || !key || !out || max_out == 0) return false;
    out[0] = 0;
    size_t klen = str_len(key);
    const char *p = json;
    while (p + klen < end) {
        if (mem_eq(p, key, klen)) {
            p += klen;
            while (p < end && (*p == ' ' || *p == '\t' || *p == ':')) p++;
            if (p < end && *p == '"') {
                p++;
                size_t i = 0;
                while (p < end && *p != '"' && i < max_out - 1) {
                    if (*p == '\\' && p + 1 < end) p++;
                    out[i++] = *p++;
                }
                out[i] = 0;
                return true;
            }
        }
        p++;
    }
    return false;
}

static bool parse_ipv4_prefix(const char *s, uint8_t out_ip[4], uint16_t *out_port)
{
    if (!s || !out_ip) return false;
    uint32_t v[4] = {0, 0, 0, 0};
    int part = 0;
    bool has_d = false;
    size_t i = 0;
    for (; s[i]; i++) {
        char c = s[i];
        if (c >= '0' && c <= '9') {
            v[part] = v[part] * 10u + (uint32_t)(c - '0');
            if (v[part] > 255u) return false;
            has_d = true;
        } else if (c == '.') {
            if (!has_d || part >= 3) return false;
            part++;
            has_d = false;
        } else if (c == '/' || c == ':' || c == '"') {
            break;
        } else {
            return false;
        }
    }
    if (part != 3 || !has_d) return false;
    for (int k = 0; k < 4; k++) out_ip[k] = (uint8_t)v[k];
    if (out_port) {
        *out_port = 0;
        if (s[i] == ':') {
            uint32_t port = 0;
            for (i++; s[i] >= '0' && s[i] <= '9'; i++) {
                port = port * 10u + (uint32_t)(s[i] - '0');
            }
            if (port <= 65535u) *out_port = (uint16_t)port;
        }
    }
    return true;
}

/* Find matching closing bracket/brace for JSON object/array */
static const char *json_find_matching(const char *open_pos, const char *end, char open_ch, char close_ch)
{
    int depth = 0;
    bool in_str = false;
    for (const char *p = open_pos; p < end; p++) {
        char c = *p;
        if (in_str) {
            if (c == '\\' && p + 1 < end) p++;
            else if (c == '"') in_str = false;
        } else {
            if (c == '"') in_str = true;
            else if (c == open_ch) depth++;
            else if (c == close_ch) {
                depth--;
                if (depth == 0) return p + 1;
            }
        }
    }
    return end;
}

static int peer_rank_score(const ts_peer_slot_t *s)
{
    if (!s || !s->info.valid) return -1;
    int score = 0;
    if (s->info.online) score += 4;
    if (s->info.endpoint_ip[0] != 0) score += 2;
    return score;
}

static bool parse_map_response(const char *json, size_t len)
{
    const char *end = json + len;

    wifi_status_t wst;
    wifi_get_status(&wst);
    uint32_t my_lan_u32 = ((uint32_t)wst.ip[0] << 24) | ((uint32_t)wst.ip[1] << 16) |
                          ((uint32_t)wst.ip[2] << 8)  | (uint32_t)wst.ip[3];
    uint32_t my_mask_u32 = ((uint32_t)wst.netmask[0] << 24) | ((uint32_t)wst.netmask[1] << 16) |
                           ((uint32_t)wst.netmask[2] << 8)  | (uint32_t)wst.netmask[3];
    if (my_mask_u32 == 0) my_mask_u32 = 0xFFFFFF00u;

    /* 1. Parse self "Node":{...} */
    const char *node_pos = json_find_range(json, end, "\"Node\":{");
    if (node_pos) {
        const char *node_obj = node_pos + 7;
        const char *node_end = json_find_matching(node_obj, end, '{', '}');
        const char *addrs = json_find_range(node_obj, node_end, "\"Addresses\":[");
        if (addrs) {
            const char *q = json_find_range(addrs + 13, node_end, "\"100.");
            if (q) {
                parse_ipv4_prefix(q + 1, s_my_ip, NULL);
            }
        }
    }

    /* 2. Parse "PeersChanged":[...] (ts2021 v109) or "Peers":[...] */
    const char *peers_pos = json_find_range(json, end, "\"PeersChanged\":[");
    size_t peers_key_len = 15u;
    if (!peers_pos) {
        peers_pos = json_find_range(json, end, "\"Peers\":[");
        peers_key_len = 8u;
    }
    if (peers_pos) {
        const char *arr_start = peers_pos + peers_key_len;
        const char *arr_end   = json_find_matching(arr_start, end, '[', ']');
        const char *p = arr_start + 1;

        s_peer_count = 0;
        while (p < arr_end && s_peer_count < TAILSCALE_MAX_PEERS) {
            while (p < arr_end && *p != '{') p++;
            if (p >= arr_end) break;
            const char *obj_end = json_find_matching(p, arr_end, '{', '}');

            ts_peer_slot_t *slot = &s_peers[s_peer_count];
            mem_zero(slot, sizeof(*slot));

            /* NodeKey ("Key":"nodekey:<64hex>") */
            char key_str[80];
            if (json_extract_string(p, obj_end, "\"Key\"", key_str, sizeof(key_str)) &&
                str_starts_with(key_str, "nodekey:") && str_len(key_str + 8) == 64) {
                hex_to_bytes(key_str + 8, slot->info.node_pub, 32);
            }

            /* DiscoKey ("DiscoKey":"discokey:<64hex>") */
            char dkey_str[80];
            if (json_extract_string(p, obj_end, "\"DiscoKey\"", dkey_str, sizeof(dkey_str)) &&
                str_starts_with(dkey_str, "discokey:") && str_len(dkey_str + 9) == 64) {
                hex_to_bytes(dkey_str + 9, slot->info.disco_pub, 32);
                nacl_box_shared_key(slot->disco_shared, s_disco_priv, slot->info.disco_pub);
            }

            /* Tailscale IPv4 from "Addresses":["100.x.y.z/32", ...] */
            const char *addrs = json_find_range(p, obj_end, "\"Addresses\":[");
            if (addrs) {
                const char *q = json_find_range(addrs + 13, obj_end, "\"100.");
                if (q) {
                    parse_ipv4_prefix(q + 1, slot->info.tailscale_ip, NULL);
                }
            }

            /* Primary MagicDNS name from "Name":"tsuginosuke.tail3dd24e.ts.net." and OS Hostname from "Hostname":"..." */
            char dns_name[64] = "";
            char os_host[48]  = "";
            json_extract_string(p, obj_end, "\"Name\"", dns_name, sizeof(dns_name));
            json_extract_string(p, obj_end, "\"Hostname\"", os_host, sizeof(os_host));
            for (size_t k = 0; dns_name[k]; k++) {
                if (dns_name[k] == '.') { dns_name[k] = 0; break; }
            }
            for (size_t k = 0; os_host[k]; k++) {
                if (os_host[k] == '.') { os_host[k] = 0; break; }
            }
            if (dns_name[0]) {
                str_copy(slot->info.hostname, sizeof(slot->info.hostname), dns_name);
                str_copy(slot->alt_hostname, sizeof(slot->alt_hostname), os_host);
            } else {
                str_copy(slot->info.hostname, sizeof(slot->info.hostname), os_host);
            }

            /* Online status */
            slot->info.online = (json_find_range(p, obj_end, "\"Online\":true") != NULL);

            /*
             * Endpoints selection:
             *  Score 3: Same local subnet as Tab5's Wi-Fi IP (e.g. 192.168.68.x/22)
             *  Score 2: Public IPv4 (STUN / WAN endpoint, e.g. 219.104.122.252:41641 for tsuginosuke)
             *  Score 1: Remote private IP (10.x / 172.16-31.x / other 192.168.x)
             */
            const char *eps = json_find_range(p, obj_end, "\"Endpoints\":[");
            if (eps) {
                const char *eps_end = json_find_matching(eps + 12, obj_end, '[', ']');
                const char *ep_cur = eps + 13;
                uint8_t best_ip[4] = {0, 0, 0, 0};
                uint16_t best_port = 0;
                int best_score = 0;
                while (ep_cur < eps_end) {
                    while (ep_cur < eps_end && *ep_cur != '"') ep_cur++;
                    if (ep_cur >= eps_end) break;
                    ep_cur++;
                    uint8_t cand_ip[4];
                    uint16_t cand_port = 0;
                    if (parse_ipv4_prefix(ep_cur, cand_ip, &cand_port) && cand_port > 0) {
                        uint32_t cand_u32 = ((uint32_t)cand_ip[0] << 24) | ((uint32_t)cand_ip[1] << 16) |
                                            ((uint32_t)cand_ip[2] << 8)  | (uint32_t)cand_ip[3];
                        bool same_subnet = (my_lan_u32 != 0) &&
                                           ((cand_u32 & my_mask_u32) == (my_lan_u32 & my_mask_u32));
                        bool is_private = (cand_ip[0] == 192u && cand_ip[1] == 168u) ||
                                          (cand_ip[0] == 10u) ||
                                          (cand_ip[0] == 172u && (cand_ip[1] >= 16u && cand_ip[1] <= 31u)) ||
                                          (cand_ip[0] == 127u) || (cand_ip[0] == 100u);
                        int score = same_subnet ? 3 : (!is_private ? 2 : 1);
                        if (score > best_score) {
                            mem_copy(best_ip, cand_ip, 4);
                            best_port = cand_port;
                            best_score = score;
                        }
                    }
                    while (ep_cur < eps_end && *ep_cur != '"') ep_cur++;
                    if (ep_cur < eps_end) ep_cur++;
                }
                if (best_ip[0] != 0) {
                    mem_copy(slot->info.endpoint_ip, best_ip, 4);
                    slot->info.endpoint_port = best_port;
                }
            }

            if (slot->info.tailscale_ip[0] == 100u) {
                if (!slot->info.hostname[0]) {
                    ksnprintf(slot->info.hostname, sizeof(slot->info.hostname), "peer-%u-%u",
                              slot->info.tailscale_ip[2], slot->info.tailscale_ip[3]);
                }
                slot->info.valid = true;
                s_peer_count++;
            }

            p = obj_end;
        }

        /* Sort peers so online peers with direct endpoints appear at the top of `tailscale status` & GUI */
        for (int i = 1; i < s_peer_count; i++) {
            ts_peer_slot_t tmp = s_peers[i];
            int sc = peer_rank_score(&tmp);
            int j = i - 1;
            while (j >= 0 && peer_rank_score(&s_peers[j]) < sc) {
                s_peers[j + 1] = s_peers[j];
                j--;
            }
            s_peers[j + 1] = tmp;
        }
    }

    return s_my_ip[0] == 100u;
}

/* Perform `/machine/register` and `/machine/map` with controlplane.tailscale.com */
static bool ts_sync_control_plane(void)
{
    wifi_status_t wst;
    wifi_get_status(&wst);
    if (wst.state == WIFI_STATE_CONNECTED && wst.ip[0] == 0) {
        for (int i = 0; i < 50 && wst.ip[0] == 0; i++) {
            task_sleep(100);
            wifi_get_status(&wst);
        }
    }
    if (wst.state != WIFI_STATE_CONNECTED || wst.ip[0] == 0) {
        ts_set_msg("Waiting for Wi-Fi connection (run 'wifi' to connect)...");
        return false;
    }

    ts_ctrl_conn_t *ctrl = (ts_ctrl_conn_t *)kmalloc_psram(sizeof(ts_ctrl_conn_t));
    if (!ctrl) ctrl = (ts_ctrl_conn_t *)kmalloc(sizeof(ts_ctrl_conn_t));
    if (!ctrl) return false;

    size_t resp_cap = 524288u; /* 512 KB in PSRAM for large Tailnets (50+ peers) */
    uint8_t *resp_buf = (uint8_t *)kmalloc_psram(resp_cap);
    if (!resp_buf) {
        resp_cap = 65536u;
        resp_buf = (uint8_t *)kmalloc(resp_cap);
    }
    if (!resp_buf) {
        kfree(ctrl);
        return false;
    }

    ts_set_msg("Connecting to controlplane.tailscale.com:80...");
    if (!ctrl_connect_and_handshake(ctrl)) {
        kfree(resp_buf);
        kfree(ctrl);
        return false;
    }

    char node_hex[65], disco_hex[65];
    bytes_to_hex(s_node_pub,  32, node_hex);
    bytes_to_hex(s_disco_pub, 32, disco_hex);

    /* 1. POST /machine/register on HTTP/2 stream 1 */
    char reg_json[512];
    if (s_saved_authkey[0]) {
        ksnprintf(reg_json, sizeof(reg_json),
                  "{\"Version\":109,\"NodeKey\":\"nodekey:%s\","
                  "\"Auth\":{\"AuthKey\":\"%s\"},"
                  "\"Hostinfo\":{\"Hostname\":\"%s\",\"OS\":\"meos\"}}",
                  node_hex, s_saved_authkey, s_hostname);
    } else {
        ksnprintf(reg_json, sizeof(reg_json),
                  "{\"Version\":109,\"NodeKey\":\"nodekey:%s\","
                  "\"Hostinfo\":{\"Hostname\":\"%s\",\"OS\":\"meos\"}}",
                  node_hex, s_hostname);
    }

    size_t rlen = 0;
    if (!ctrl_h2_post(ctrl, 1, "/machine/register", reg_json, resp_buf, 4096, &rlen, false)) {
        wifi_tcp_close_sock(ctrl->sock);
        kfree(resp_buf);
        kfree(ctrl);
        ts_set_msg("/machine/register request failed");
        return false;
    }
    resp_buf[rlen < 4095 ? rlen : 4095] = 0;

    char auth_url[160] = "";
    json_extract_string((const char *)resp_buf, (const char *)resp_buf + rlen,
                        "\"AuthURL\"", auth_url, sizeof(auth_url));
    bool authorized = (str_find((const char *)resp_buf, "\"MachineAuthorized\":true") != NULL) ||
                      (auth_url[0] == 0 && str_find((const char *)resp_buf, "\"Error\"") == NULL);

    if (auth_url[0] != 0 && !authorized) {
        ts_lock();
        str_copy(s_auth_url, sizeof(s_auth_url), auth_url);
        s_state = TS_STATE_NEEDS_LOGIN;
        ksnprintf(s_last_msg, sizeof(s_last_msg), "Login required: %s", auth_url);
        ts_unlock();
        kprintf("tailscale: Authorize node at: %s\n", auth_url);

        /* Follow up with /machine/register (Followup: auth_url) on stream 3 to wait for user approval */
        ksnprintf(reg_json, sizeof(reg_json),
                  "{\"Version\":109,\"NodeKey\":\"nodekey:%s\","
                  "\"Followup\":\"%s\","
                  "\"Hostinfo\":{\"Hostname\":\"%s\",\"OS\":\"meos\"}}",
                  node_hex, auth_url, s_hostname);
        rlen = 0;
        if (!ctrl_h2_post(ctrl, 3, "/machine/register", reg_json, resp_buf, 4096, &rlen, false)) {
            wifi_tcp_close_sock(ctrl->sock);
            kfree(resp_buf);
            kfree(ctrl);
            return false;
        }
        resp_buf[rlen < 4095 ? rlen : 4095] = 0;
        if (str_find((const char *)resp_buf, "\"MachineAuthorized\":true") == NULL) {
            wifi_tcp_close_sock(ctrl->sock);
            kfree(resp_buf);
            kfree(ctrl);
            return false;
        }
    }

    /* 2. POST /machine/map on HTTP/2 stream 5 (Stream=true ensures full initial Peer list is sent) */
    ts_set_msg("Fetching Tailnet map (/machine/map)...");
    char map_json[512];
    ksnprintf(map_json, sizeof(map_json),
              "{\"Version\":109,\"Compress\":\"\",\"KeepAlive\":true,\"Stream\":true,"
              "\"NodeKey\":\"nodekey:%s\",\"DiscoKey\":\"discokey:%s\","
              "\"Endpoints\":[\"%u.%u.%u.%u:%u\"],"
              "\"Hostinfo\":{\"Hostname\":\"%s\",\"OS\":\"meos\"}}",
              node_hex, disco_hex,
              wst.ip[0], wst.ip[1], wst.ip[2], wst.ip[3], TS_WG_PORT,
              s_hostname);

    rlen = 0;
    bool map_ok = ctrl_h2_post(ctrl, 5, "/machine/map", map_json, resp_buf, resp_cap - 1, &rlen, true);

    if (!map_ok || rlen <= 4u) {
        wifi_tcp_close_sock(ctrl->sock);
        kfree(resp_buf);
        kfree(ctrl);
        ts_set_msg("/machine/map returned empty response");
        return false;
    }

    uint32_t json_len = get_le32(resp_buf);
    if (4u + json_len > rlen) json_len = (uint32_t)(rlen - 4u);
    resp_buf[4u + json_len] = 0;

    ts_lock();
    bool parsed = parse_map_response((const char *)(resp_buf + 4), json_len);
    if (parsed) {
        s_auth_url[0] = 0;
        s_state = TS_STATE_RUNNING;
        ts_save_keys();
        wifi_set_udp_rx_callback(TS_WG_PORT, ts_udp_rx_callback);
        wifi_set_tun_callbacks(s_my_ip, ts_tun_tx_callback, ts_tun_resolve_callback);
        ksnprintf(s_last_msg, sizeof(s_last_msg),
                  "Connected: %u.%u.%u.%u (%d peers)",
                  s_my_ip[0], s_my_ip[1], s_my_ip[2], s_my_ip[3], s_peer_count);
        kprintf("tailscale: Joined tailnet as %s (%u.%u.%u.%u), %d peers\n",
                s_hostname, s_my_ip[0], s_my_ip[1], s_my_ip[2], s_my_ip[3], s_peer_count);

        /* Proactively send Disco Ping to local-subnet peers with known endpoints */
        for (int i = 0; i < s_peer_count; i++) {
            if (s_peers[i].info.valid && s_peers[i].info.online &&
                s_peers[i].info.endpoint_ip[0] == 192u && s_peers[i].info.endpoint_ip[1] == 168u) {
                disco_send_ping_locked(&s_peers[i]);
            }
        }
    } else {
        wifi_tcp_close_sock(ctrl->sock);
        ts_set_msg("Failed to parse MapResponse JSON");
    }
    ts_unlock();

    kfree(resp_buf);
    kfree(ctrl);
    return parsed;
}

/* =========================================================================
 * 9. Background Tailscale Daemon Task & Public API
 * ========================================================================= */

static void tailscale_daemon_task(void *arg)
{
    (void)arg;
    uint32_t tick = 0;
    while (s_want_up) {
        if (s_state != TS_STATE_RUNNING) {
            ts_sync_control_plane();
            for (int i = 0; i < 8 && s_want_up && s_state != TS_STATE_RUNNING; i++) {
                task_sleep(100);
            }
        } else {
            /* Drain controlplane keepalive frames on TS_SOCK_CTRL so session stays online */
            static uint8_t s_ctrl_drain[256];
            wifi_tcp_recv_sock(TS_SOCK_CTRL, s_ctrl_drain, sizeof(s_ctrl_drain), 0);

            /* Periodic keepalive & Disco health check every 10 seconds (<15s magicsock trust window) */
            if (tick > 0 && (tick % 100u) == 0) {
                ts_lock();
                for (int i = 0; i < s_peer_count; i++) {
                    ts_peer_slot_t *p = &s_peers[i];
                    if (!p->info.valid) continue;
                    if (p->info.wg_established) {
                        disco_send_ping_locked(p);
                        wg_send_keepalive_locked(p);
                    } else if (p->info.online && p->info.endpoint_ip[0] == 192u && p->info.endpoint_ip[1] == 168u) {
                        disco_send_ping_locked(p);
                    }
                }
                ts_unlock();
            }
            tick++;
            task_sleep(100);
        }
    }
    wifi_tcp_close_sock(TS_SOCK_CTRL);
    s_daemon_running = false;
}

static void tailscale_boot_task(void *unused)
{
    (void)unused;
    ts_ensure_lock();
    ts_lock();
    ts_load_or_gen_keys();
    /* Register MagicDNS & TUN callbacks immediately so any hostname lookup can auto-sync Tailscale */
    wifi_set_udp_rx_callback(TS_WG_PORT, ts_udp_rx_callback);
    wifi_set_tun_callbacks(s_my_ip, ts_tun_tx_callback, ts_tun_resolve_callback);
    s_want_up = true;
    if (!s_daemon_running) {
        s_daemon_running = true;
        task_create_stack("tailscaled", tailscale_daemon_task, NULL, TASK_PRIO_DEFAULT, 24576);
    }
    ts_unlock();

    task_stack_info_t stack;
    if (task_stack_inspect(task_current()->id, &stack) == 0) {
        kprintf("tailscale: boot stack %u/%u bytes, guard %s\n",
                (unsigned)stack.used, (unsigned)stack.size,
                stack.guard_ok ? "OK" : "FAILED");
    }
}

void tailscale_boot_start(void)
{
    static bool scheduled;
    if (scheduled) return;
    scheduled = true;
    if (task_create_stack("tsboot", tailscale_boot_task, NULL,
                          TASK_PRIO_DEFAULT, 16384) < 0) {
        scheduled = false;
        kprintf("tailscale: cannot schedule boot initialization\n");
    }
}

bool tailscale_up(const char *auth_key)
{
    ts_ensure_lock();
    ts_lock();
    ts_load_or_gen_keys();
    if (auth_key && auth_key[0]) {
        str_copy(s_saved_authkey, sizeof(s_saved_authkey), auth_key);
        ts_save_keys();
    }
    if (s_state != TS_STATE_RUNNING) {
        s_state = TS_STATE_CONNECTING;
        ts_set_msg("Starting Tailscale...");
    }
    s_want_up = true;
    if (!s_daemon_running) {
        s_daemon_running = true;
        task_create_stack("tailscaled", tailscale_daemon_task, NULL, TASK_PRIO_DEFAULT, 24576);
    }
    ts_unlock();

    /* Wait up to 18 seconds for either RUNNING or NEEDS_LOGIN */
    uint64_t deadline = hal_time_now_us() + 18000000ULL;
    while (hal_time_now_us() < deadline) {
        if (s_state == TS_STATE_RUNNING || s_state == TS_STATE_NEEDS_LOGIN) {
            return true;
        }
        task_sleep(100);
    }
    return s_state == TS_STATE_RUNNING || s_state == TS_STATE_NEEDS_LOGIN;
}

void tailscale_down(void)
{
    ts_ensure_lock();
    s_want_up = false;
    ts_lock();
    wifi_set_udp_rx_callback(0, NULL);
    wifi_set_tun_callbacks(NULL, NULL, NULL);
    s_state = TS_STATE_STOPPED;
    ts_set_msg("Stopped");
    for (int i = 0; i < s_peer_count; i++) {
        s_peers[i].info.wg_established = false;
        s_peers[i].wg_hs_pending = false;
    }
    ts_unlock();
}

void tailscale_get_status(ts_status_t *out)
{
    if (!out) return;
    ts_ensure_lock();
    ts_lock();
    ts_load_or_gen_keys();
    mem_zero(out, sizeof(*out));
    out->state = s_state;
    mem_copy(out->my_ip, s_my_ip, 4);
    str_copy(out->hostname, sizeof(out->hostname), s_hostname);
    str_copy(out->auth_url, sizeof(out->auth_url), s_auth_url);
    str_copy(out->last_msg, sizeof(out->last_msg), s_last_msg);
    out->peer_count = s_peer_count;
    for (int i = 0; i < s_peer_count && i < TAILSCALE_MAX_PEERS; i++) {
        out->peers[i] = s_peers[i].info;
    }
    ts_unlock();
}

static uint16_t calc_ip_csum(const uint8_t *buf, size_t len)
{
    uint32_t sum = 0;
    for (size_t i = 0; i + 1 < len; i += 2) {
        sum += ((uint32_t)buf[i] << 8) | buf[i + 1];
    }
    if (len & 1u) sum += (uint32_t)buf[len - 1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);
    return (uint16_t)(~sum & 0xFFFFu);
}

int tailscale_ping(const char *target, uint8_t out_peer_ip[4], uint8_t out_ep_ip[4], uint16_t *out_ep_port)
{
    if (!target || !target[0]) return -1;
    uint8_t dst_ip[4];
    if (!wifi_resolve_host(target, dst_ip)) return -1;
    if (out_peer_ip) mem_copy(out_peer_ip, dst_ip, 4);

    ts_ensure_lock();
    ts_lock();
    ts_peer_slot_t *p = find_peer_by_ip_locked(dst_ip);
    if (!p || p->info.endpoint_ip[0] == 0) {
        ts_unlock();
        return -1;
    }

    mem_copy(s_ping_target_ip, dst_ip, 4);
    s_ping_done    = false;
    s_ping_waiting = true;
    s_ping_tx_us   = hal_time_now_us();

    /* 1. Send Disco Ping (always answered by Tailscale magicsock over direct UDP) */
    disco_send_ping_locked(p);

    /* 2. Also initiate WireGuard handshake or send ICMP Echo Request inside WireGuard Type-4 */
    if (!p->info.wg_established) {
        wg_send_handshake_initiation_locked(p);
    } else {
        uint8_t icmp_pkt[28];
        mem_zero(icmp_pkt, sizeof(icmp_pkt));
        icmp_pkt[0] = 0x45u;
        icmp_pkt[2] = 0u; icmp_pkt[3] = 28u;
        icmp_pkt[4] = 0x12u; icmp_pkt[5] = 0x34u;
        icmp_pkt[8] = 64u;
        icmp_pkt[9] = 1u; /* ICMP */
        mem_copy(&icmp_pkt[12], s_my_ip, 4);
        mem_copy(&icmp_pkt[16], dst_ip, 4);
        uint16_t hcsum = calc_ip_csum(icmp_pkt, 20);
        icmp_pkt[10] = (uint8_t)(hcsum >> 8);
        icmp_pkt[11] = (uint8_t)(hcsum & 0xFFu);

        icmp_pkt[20] = 8u; /* Echo Request */
        icmp_pkt[24] = 0x54u; icmp_pkt[25] = 0x53u;
        uint16_t seq = s_ping_icmp_seq++;
        icmp_pkt[26] = (uint8_t)(seq >> 8);
        icmp_pkt[27] = (uint8_t)(seq & 0xFFu);
        uint16_t icsum = calc_ip_csum(&icmp_pkt[20], 8);
        icmp_pkt[22] = (uint8_t)(icsum >> 8);
        icmp_pkt[23] = (uint8_t)(icsum & 0xFFu);
        wg_send_ipv4_to_peer_locked(p, icmp_pkt, sizeof(icmp_pkt));
    }
    ts_unlock();

    uint64_t deadline = hal_time_now_us() + 2500000ULL;
    while (hal_time_now_us() < deadline) {
        wifi_status_t wst;
        wifi_get_status(&wst); /* polls SDIO RX */
        if (s_ping_done) {
            s_ping_waiting = false;
            if (out_ep_ip) mem_copy(out_ep_ip, s_ping_pong_ip, 4);
            if (out_ep_port) *out_ep_port = s_ping_pong_port;
            return (int)s_ping_rtt_ms;
        }
        task_sleep(10);
    }

    s_ping_waiting = false;
    return -1;
}

/*/* =========================================================================
 * 10. QR Code Generator (ISO/IEC 18004 Version 3-L, 29x29) & Display Renderer
 * ========================================================================= */

#define QR_SIZE 29

static uint8_t qr_gf_mul(uint8_t a, uint8_t b)
{
    uint8_t p = 0;
    for (int i = 0; i < 8; i++) {
        if (b & 1u) p ^= a;
        uint8_t hi = (uint8_t)(a & 0x80u);
        a = (uint8_t)(a << 1);
        if (hi) a ^= 0x1Du;
        b >>= 1;
    }
    return p;
}

/* Encode an ASCII string (up to 53 bytes) into a 29x29 QR Version 3-L matrix (1 = dark, 0 = light) */
static bool qr_encode_v3L(const char *text, uint8_t modules[QR_SIZE][QR_SIZE])
{
    size_t len = str_len(text);
    if (len == 0 || len > 53u) return false;

    /* 1. Build 55 data codewords (Byte mode = 0100, 8-bit length, bytes, 0000 terminator, 0xEC/0x11 pad) */
    static uint8_t codewords[70];
    mem_zero(codewords, sizeof(codewords));

    size_t bit_pos = 0;
    #define PUT_BITS(val, nbits) do { \
        for (int _b = (nbits) - 1; _b >= 0; _b--) { \
            if (((val) >> _b) & 1u) codewords[bit_pos >> 3] |= (uint8_t)(0x80u >> (bit_pos & 7u)); \
            bit_pos++; \
        } \
    } while (0)

    PUT_BITS(0x4u, 4);            /* Byte mode indicator (0100) */
    PUT_BITS((uint32_t)len, 8);   /* Character count */
    for (size_t i = 0; i < len; i++) {
        PUT_BITS((uint8_t)text[i], 8);
    }
    for (int i = 0; i < 4 && bit_pos < 55u * 8u; i++) {
        PUT_BITS(0u, 1);          /* Terminator */
    }
    while (bit_pos & 7u) {
        PUT_BITS(0u, 1);          /* Byte-align */
    }
    uint8_t pad_byte = 0xECu;
    while ((bit_pos >> 3) < 55u) {
        codewords[bit_pos >> 3] = pad_byte;
        bit_pos += 8u;
        pad_byte = (pad_byte == 0xECu) ? 0x11u : 0xECu;
    }
    #undef PUT_BITS

    /* 2. Compute 15 Reed-Solomon EC codewords into codewords[55..69] */
    static const uint8_t rs_gen15[15] = {
        0x1d, 0xc4, 0x6f, 0xa3, 0x70, 0x4a, 0x0a, 0x69,
        0x69, 0x8b, 0x84, 0x97, 0x20, 0x86, 0x1a
    };
    uint8_t *ec = &codewords[55];
    for (int i = 0; i < 55; i++) {
        uint8_t factor = codewords[i] ^ ec[0];
        for (int j = 0; j < 14; j++) ec[j] = ec[j + 1];
        ec[14] = 0;
        for (int j = 0; j < 15; j++) {
            ec[j] ^= qr_gf_mul(rs_gen15[j], factor);
        }
    }

    /* 3. Place function patterns and reserve format areas */
    static uint8_t is_func[QR_SIZE][QR_SIZE];
    mem_zero(modules, QR_SIZE * QR_SIZE);
    mem_zero(is_func, QR_SIZE * QR_SIZE);

    /* Finder patterns at (0,0), (22,0), (0,22) + 1-module white separators */
    const int f_coords[3][2] = { {0, 0}, {QR_SIZE - 7, 0}, {0, QR_SIZE - 7} };
    for (int f = 0; f < 3; f++) {
        int fx = f_coords[f][0], fy = f_coords[f][1];
        for (int dy = -1; dy <= 7; dy++) {
            for (int dx = -1; dx <= 7; dx++) {
                int x = fx + dx, y = fy + dy;
                if (x >= 0 && x < QR_SIZE && y >= 0 && y < QR_SIZE) {
                    is_func[y][x] = 1;
                    bool dark = (dx >= 0 && dx <= 6 && dy >= 0 && dy <= 6) &&
                                (dx == 0 || dx == 6 || dy == 0 || dy == 6 ||
                                 (dx >= 2 && dx <= 4 && dy >= 2 && dy <= 4));
                    modules[y][x] = dark ? 1 : 0;
                }
            }
        }
    }

    /* Alignment pattern for Version 3 centered at (22, 22) -> top-left (20, 20) */
    for (int dy = -2; dy <= 2; dy++) {
        for (int dx = -2; dx <= 2; dx++) {
            int x = 22 + dx, y = 22 + dy;
            is_func[y][x] = 1;
            bool dark = (dx == -2 || dx == 2 || dy == -2 || dy == 2 || (dx == 0 && dy == 0));
            modules[y][x] = dark ? 1 : 0;
        }
    }

    /* Timing patterns on row 6 and col 6 */
    for (int i = 8; i < QR_SIZE - 8; i++) {
        is_func[6][i] = 1; modules[6][i] = (i % 2 == 0) ? 1 : 0;
        is_func[i][6] = 1; modules[i][6] = (i % 2 == 0) ? 1 : 0;
    }

    /* Dark module at (x=8, y=4*3+9 = 21) and reserve format info cells */
    for (int i = 0; i < 9; i++) {
        is_func[8][i] = 1;
        is_func[i][8] = 1;
    }
    for (int i = 0; i < 8; i++) {
        is_func[8][QR_SIZE - 1 - i] = 1;
        is_func[QR_SIZE - 1 - i][8] = 1;
    }
    modules[QR_SIZE - 8][8] = 1; /* Dark module */

    /* 4. Place 70 codewords (560 bits) in upward/downward 2-column zigzag */
    size_t bidx = 0;
    for (int right = QR_SIZE - 1; right >= 1; right -= 2) {
        if (right == 6) right = 5; /* Skip vertical timing column 6 */
        bool upward = (((right + 1) & 2) == 0);
        for (int v = 0; v < QR_SIZE; v++) {
            int y = upward ? (QR_SIZE - 1 - v) : v;
            for (int j = 0; j < 2; j++) {
                int x = right - j;
                if (!is_func[y][x]) {
                    uint8_t bit = 0;
                    if (bidx < 70u * 8u) {
                        bit = (codewords[bidx >> 3] >> (7u - (bidx & 7u))) & 1u;
                        bidx++;
                    }
                    /* Apply Mask Pattern 0: ((x + y) % 2 == 0) */
                    if (((x + y) & 1) == 0) bit ^= 1u;
                    modules[y][x] = bit;
                }
            }
        }
    }

    /* 5. Write Format Information for ECC Level L (01) + Mask 0 (000) -> 0x77C4 */
    const uint16_t fmt = 0x77C4u;
    static const uint8_t fmt_y1[15] = { 8,8,8,8,8,8,8,8, 7,5,4,3,2,1,0 };
    static const uint8_t fmt_x1[15] = { 0,1,2,3,4,5,7,8, 8,8,8,8,8,8,8 };
    for (int i = 0; i < 15; i++) {
        uint8_t mbit = (fmt >> (14 - i)) & 1u;
        modules[fmt_y1[i]][fmt_x1[i]] = mbit;
        if (i < 7) {
            modules[QR_SIZE - 1 - i][8] = mbit;
        } else {
            modules[8][QR_SIZE - 15 + i] = mbit;
        }
    }
    return true;
}

static uint8_t s_qr_mod[QR_SIZE][QR_SIZE];

static void qr_draw_on_lcd(int px, int py, int scale, const char *url)
{
    if (!qr_encode_v3L(url, s_qr_mod)) return;

    int quiet = 3;
    int total = (QR_SIZE + quiet * 2) * scale;
    display_fill_rect(px, py, total, total, COLOR_WHITE);

    for (int y = 0; y < QR_SIZE; y++) {
        for (int x = 0; x < QR_SIZE; x++) {
            if (s_qr_mod[y][x]) {
                display_fill_rect(px + (x + quiet) * scale,
                                  py + (y + quiet) * scale,
                                  scale, scale, COLOR_BLACK);
            }
        }
    }
}

static void qr_print_console(const char *url)
{
    if (!qr_encode_v3L(url, s_qr_mod)) return;

    kprintf("\n  Scan QR code with iPhone Camera to authorize this Tab5:\n\n");
    for (int y = -2; y < QR_SIZE + 2; y += 2) {
        kprintf("    ");
        for (int x = -2; x < QR_SIZE + 2; x++) {
            bool top = (y >= 0 && y < QR_SIZE && x >= 0 && x < QR_SIZE) ? (s_qr_mod[y][x] != 0) : false;
            bool bot = (y + 1 >= 0 && y + 1 < QR_SIZE && x >= 0 && x < QR_SIZE) ? (s_qr_mod[y + 1][x] != 0) : false;
            if (!top && !bot)      kprintf("\xe2\x96\x88"); /* Full block = white/light */
            else if (!top && bot)  kprintf("\xe2\x96\x80"); /* Upper half block */
            else if (top && !bot)  kprintf("\xe2\x96\x84"); /* Lower half block */
            else                   kprintf(" ");            /* Space = dark module */
        }
        kprintf("\n");
    }
    kprintf("\n");
}

/* =========================================================================
 * 11. Full-Screen Graphical Tailscale Manager (`tailscale` / `run tailscale`)
 * ========================================================================= */

static ts_status_t s_ui_st;

static void ts_draw_pad(int cx, int cy, int width, const char *s, bool rev)
{
    char buf[120];
    int i = 0;
    if (width >= (int)sizeof(buf)) width = (int)sizeof(buf) - 1;
    if (s) {
        for (; s[i] && i < width; i++) buf[i] = s[i];
    }
    for (; i < width; i++) buf[i] = ' ';
    buf[width] = 0;
    if (rev) dq_text_rev(cx, cy, buf);
    else     dq_text(cx, cy, buf);
}

void tailscale_app_main(void)
{
    int sel = 0;
    char status_bar[96] = "Enter/P:Ping  U:Up/Sync  W:Wi-Fi  K:AuthKey  D:Down  Q/Esc:Quit";

    ts_ensure_lock();
    ts_lock();
    ts_load_or_gen_keys();
    if (s_state == TS_STATE_STOPPED) {
        s_state = TS_STATE_CONNECTING;
        ts_set_msg("Starting Tailscale...");
    }
    s_want_up = true;
    if (!s_daemon_running) {
        s_daemon_running = true;
        task_create_stack("tailscaled", tailscale_daemon_task, NULL, TASK_PRIO_DEFAULT, 24576);
    }
    ts_unlock();

    uint64_t last_draw_us = 0;
    bool full_redraw = true;
    bool prev_had_qr = false;
    bool dirty = true;

    for (;;) {
        uint64_t now = hal_time_now_us();
        if (dirty || (now - last_draw_us >= 600000ULL)) {
            tailscale_get_status(&s_ui_st);
            if (sel >= s_ui_st.peer_count) sel = s_ui_st.peer_count > 0 ? s_ui_st.peer_count - 1 : 0;

            bool has_qr = (s_ui_st.auth_url[0] != 0 && s_ui_st.state != TS_STATE_RUNNING);
            if (has_qr != prev_had_qr) {
                full_redraw = true;
                prev_had_qr = has_qr;
            }

            int cols = dq_cols();
            int rows = dq_rows();
            int inner_w = cols - 4;
            if (inner_w > 110) inner_w = 110;

            if (full_redraw) {
                dq_clear();
                dq_window(0, 0, cols, 5);
                dq_window(0, 5, cols, rows - 9);
                dq_window(0, rows - 4, cols, 4);
                full_redraw = false;
            }

            /* Header window */
            ts_draw_pad(2, 1, inner_w, "MeOS TAILSCALE MESH VPN MANAGER (WireGuard + ts2021 Control Plane)", false);

            const char *state_str = "STOPPED";
            if (s_ui_st.state == TS_STATE_RUNNING)          state_str = "RUNNING";
            else if (s_ui_st.state == TS_STATE_CONNECTING)  state_str = "CONNECTING";
            else if (s_ui_st.state == TS_STATE_NEEDS_LOGIN) state_str = "NEEDS LOGIN";
            else if (s_ui_st.state == TS_STATE_ERROR)       state_str = "ERROR";

            char hdr1[120], hdr2[120];
            ksnprintf(hdr1, sizeof(hdr1), "State: %-11s | Node: %s (%u.%u.%u.%u) | Peers: %d",
                      state_str, s_ui_st.hostname,
                      s_ui_st.my_ip[0], s_ui_st.my_ip[1], s_ui_st.my_ip[2], s_ui_st.my_ip[3],
                      s_ui_st.peer_count);
            ts_draw_pad(2, 2, inner_w, hdr1, false);

            if (has_qr) {
                ksnprintf(hdr2, sizeof(hdr2), "Scan QR with iPhone or open: %s", s_ui_st.auth_url);
            } else {
                ksnprintf(hdr2, sizeof(hdr2), "Status : %s", s_ui_st.last_msg);
            }
            ts_draw_pad(2, 3, inner_w, hdr2, has_qr);

            if (has_qr) {
                /* Render large scannable QR code on the Tab5 IPS display + instructions */
                ts_draw_pad(4, 7, 46, "=== iPhone QR Code Login ===", false);
                ts_draw_pad(4, 9, 46, "1. Open Camera app on your iPhone", false);
                ts_draw_pad(4, 11, 46, "2. Point it at the QR code on the right ->", false);
                ts_draw_pad(4, 13, 46, "3. Tap 'login.tailscale.com' & Connect!", false);
                ts_draw_pad(4, 15, 46, "   (Tab5 will automatically join tailnet)", false);
                ts_draw_pad(4, 18, 48, s_ui_st.auth_url, true);

                int qr_scale = (DISPLAY_HEIGHT >= 480) ? 9 : 4;
                int qr_px_size = (QR_SIZE + 6) * qr_scale;
                int qr_x = DISPLAY_WIDTH - qr_px_size - 60;
                int qr_y = (DISPLAY_HEIGHT - qr_px_size) / 2 + 8;
                if (qr_x < 16) qr_x = (DISPLAY_WIDTH - qr_px_size) / 2;
                qr_draw_on_lcd(qr_x, qr_y, qr_scale, s_ui_st.auth_url);
            } else {
                /* Peer Table */
                ts_draw_pad(2, 6, inner_w,
                            "  HOSTNAME               TAILSCALE IP     DIRECT ENDPOINT        STATUS / TUNNEL",
                            false);

                int max_list_rows = rows - 12;
                int top = 0;
                if (sel >= max_list_rows) top = sel - max_list_rows + 1;
                for (int r = 0; r < max_list_rows; r++) {
                    int py = 8 + r;
                    int idx = top + r;
                    if (idx < s_ui_st.peer_count) {
                        const ts_peer_info_t *p = &s_ui_st.peers[idx];
                        char ip_s[20], ep_s[26], st_s[36], line[120];
                        ksnprintf(ip_s, sizeof(ip_s), "%u.%u.%u.%u",
                                  p->tailscale_ip[0], p->tailscale_ip[1], p->tailscale_ip[2], p->tailscale_ip[3]);
                        if (p->endpoint_ip[0]) {
                            ksnprintf(ep_s, sizeof(ep_s), "%u.%u.%u.%u:%u",
                                      p->endpoint_ip[0], p->endpoint_ip[1], p->endpoint_ip[2], p->endpoint_ip[3],
                                      p->endpoint_port);
                        } else {
                            str_copy(ep_s, sizeof(ep_s), "derp-relay");
                        }

                        if (p->wg_established) {
                            ksnprintf(st_s, sizeof(st_s), "WG TUNNEL (TX:%u RX:%u)", p->tx_packets, p->rx_packets);
                        } else if (p->last_rtt_ms > 0) {
                            ksnprintf(st_s, sizeof(st_s), "DIRECT (%u ms)", p->last_rtt_ms);
                        } else if (p->online) {
                            str_copy(st_s, sizeof(st_s), "ONLINE");
                        } else {
                            str_copy(st_s, sizeof(st_s), "IDLE");
                        }

                        ksnprintf(line, sizeof(line), "%c %-22s %-16s %-22s %s",
                                  (idx == sel) ? '>' : ' ', p->hostname, ip_s, ep_s, st_s);
                        ts_draw_pad(2, py, inner_w, line, idx == sel);
                    } else if (r == 0 && s_ui_st.peer_count == 0) {
                        ts_draw_pad(2, py, inner_w, "  (No peers loaded yet - press W for Wi-Fi, U to sync, or K for AuthKey)", false);
                    } else {
                        ts_draw_pad(2, py, inner_w, "", false);
                    }
                }
            }

            /* Footer window */
            ts_draw_pad(2, rows - 3, inner_w, status_bar, false);
            ts_draw_pad(2, rows - 2, inner_w,
                        "Up/Down:Select  Enter/P:Ping  U:Up/Sync  W:Wi-Fi  K:AuthKey  D:Down  Q/Esc:Quit",
                        false);

            last_draw_us = now;
            dirty = false;
        }

        int k = dq_pollkey();
        if (k != DQK_NONE) {
            if (k == 'q' || k == 'Q' || k == DQK_ESC) {
                break;
            } else if ((k == DQK_UP || k == 'k') && sel > 0) {
                sel--;
                dirty = true;
            } else if ((k == DQK_DOWN || k == 'j') && sel + 1 < s_ui_st.peer_count) {
                sel++;
                dirty = true;
            } else if (k == 'u' || k == 'U') {
                str_copy(status_bar, sizeof(status_bar), "Syncing with controlplane.tailscale.com...");
                s_want_up = true;
                dirty = true;
            } else if (k == 'w' || k == 'W') {
                dq_clear();
                wifi_app_run();
                full_redraw = true;
                dirty = true;
            } else if (k == 'K') {
                char key_buf[96] = "";
                if (dq_input("Tailscale AuthKey (tskey-auth-...):", key_buf, sizeof(key_buf)) && key_buf[0]) {
                    tailscale_up(key_buf);
                }
                full_redraw = true;
                dirty = true;
            } else if (k == 'd' || k == 'D') {
                tailscale_down();
                str_copy(status_bar, sizeof(status_bar), "Tailscale stopped.");
                full_redraw = true;
                dirty = true;
            } else if ((k == DQK_ENTER || k == 'p' || k == 'P') && s_ui_st.peer_count > 0) {
                const ts_peer_info_t *p = &s_ui_st.peers[sel];
                uint8_t ep_ip[4] = {0};
                uint16_t ep_port = 0;
                int rtt = tailscale_ping(p->hostname, NULL, ep_ip, &ep_port);
                if (rtt >= 0) {
                    ksnprintf(status_bar, sizeof(status_bar),
                              "pong from %s (%u.%u.%u.%u) via %u.%u.%u.%u:%u in %d ms",
                              p->hostname,
                              p->tailscale_ip[0], p->tailscale_ip[1], p->tailscale_ip[2], p->tailscale_ip[3],
                              ep_ip[0], ep_ip[1], ep_ip[2], ep_ip[3], ep_port, rtt);
                } else {
                    ksnprintf(status_bar, sizeof(status_bar),
                              "ping %s (%u.%u.%u.%u): timed out",
                              p->hostname,
                              p->tailscale_ip[0], p->tailscale_ip[1], p->tailscale_ip[2], p->tailscale_ip[3]);
                }
                dirty = true;
            }
        }
        task_sleep(25);
    }
}

/* =========================================================================
 * 12. CLI Command Handler (`tailscale [up|down|status|ip|ping|gui]`)
 * ========================================================================= */

static void print_status_cli(void)
{
    tailscale_get_status(&s_ui_st);

    if (s_ui_st.state == TS_STATE_STOPPED) {
        kprintf("Tailscale is stopped. Run 'tailscale up' to connect.\n");
        return;
    }
    if (s_ui_st.auth_url[0]) {
        kprintf("To authenticate, visit:\n  %s\n", s_ui_st.auth_url);
        qr_print_console(s_ui_st.auth_url);
        if (DISPLAY_HEIGHT >= 480) {
            qr_draw_on_lcd(DISPLAY_WIDTH - (QR_SIZE + 6) * 8 - 24, 60, 8, s_ui_st.auth_url);
        }
    }
    if (s_ui_st.my_ip[0]) {
        kprintf("%u.%u.%u.%u   %-22s %-10s active; direct\n",
                s_ui_st.my_ip[0], s_ui_st.my_ip[1], s_ui_st.my_ip[2], s_ui_st.my_ip[3],
                s_ui_st.hostname, "meos");
    } else {
        kprintf("State: %s\n", s_ui_st.last_msg);
    }

    for (int i = 0; i < s_ui_st.peer_count; i++) {
        const ts_peer_info_t *p = &s_ui_st.peers[i];
        char ip_s[20];
        ksnprintf(ip_s, sizeof(ip_s), "%u.%u.%u.%u",
                  p->tailscale_ip[0], p->tailscale_ip[1], p->tailscale_ip[2], p->tailscale_ip[3]);
        if (p->endpoint_ip[0]) {
            kprintf("%-15s %-22s %-7s direct %u.%u.%u.%u:%u%s\n",
                    ip_s, p->hostname, p->online ? "online" : "idle",
                    p->endpoint_ip[0], p->endpoint_ip[1], p->endpoint_ip[2], p->endpoint_ip[3],
                    p->endpoint_port,
                    p->wg_established ? " [wg-tunnel]" : "");
        } else {
            kprintf("%-15s %-22s %-7s relay\n",
                    ip_s, p->hostname, p->online ? "online" : "offline");
        }
    }
}



void tailscale_cmd_main(int argc, char **argv)
{
    if (argc < 2 || str_eq(argv[1], "gui")) {
        fbcon_pause(true);
        task_sleep(80);
        tailscale_app_main();
        fbcon_pause(false);
        return;
    }

    const char *sub = argv[1];
    if (str_eq(sub, "up")) {
        const char *authkey = NULL;
        for (int i = 2; i < argc; i++) {
            if (str_starts_with(argv[i], "--authkey=")) {
                authkey = argv[i] + 10;
            } else if ((str_eq(argv[i], "--authkey") || str_eq(argv[i], "-k")) && i + 1 < argc) {
                authkey = argv[++i];
            } else if (str_starts_with(argv[i], "tskey-")) {
                authkey = argv[i];
            }
        }
        kprintf("Starting Tailscale client on MeOS...\n");
        tailscale_up(authkey);
        print_status_cli();
        return;
    }

    if (str_eq(sub, "down")) {
        tailscale_down();
        kprintf("Tailscale disconnected.\n");
        return;
    }

    if (str_eq(sub, "status")) {
        print_status_cli();
        return;
    }

    if (str_eq(sub, "qr") || str_eq(sub, "login")) {
        tailscale_get_status(&s_ui_st);
        const char *url = (argc >= 3 && argv[2][0]) ? argv[2] : s_ui_st.auth_url;
        if (!url || !url[0]) {
            tailscale_up(NULL);
            tailscale_get_status(&s_ui_st);
            url = s_ui_st.auth_url;
        }
        if (!url || !url[0]) {
            kprintf("No AuthURL available yet (%s).\n", s_ui_st.last_msg);
            return;
        }
        kprintf("Tailscale Login URL:\n  %s\n", url);
        qr_print_console(url);
        if (DISPLAY_HEIGHT >= 480) {
            qr_draw_on_lcd(DISPLAY_WIDTH - (QR_SIZE + 6) * 8 - 24, 60, 8, url);
        }
        return;
    }

    if (str_eq(sub, "ip")) {
        tailscale_get_status(&s_ui_st);
        if (s_ui_st.my_ip[0]) {
            kprintf("%u.%u.%u.%u\n", s_ui_st.my_ip[0], s_ui_st.my_ip[1], s_ui_st.my_ip[2], s_ui_st.my_ip[3]);
        } else {
            kprintf("Tailscale IP not assigned yet (%s)\n", s_ui_st.last_msg);
        }
        return;
    }

    if (str_eq(sub, "ping")) {
        if (argc < 3) {
            kprintf("Usage: tailscale ping <hostname-or-100.x.y.z>\n");
            return;
        }
        const char *target = argv[2];
        for (int i = 0; i < 3; i++) {
            uint8_t peer_ip[4] = {0}, ep_ip[4] = {0};
            uint16_t ep_port = 0;
            int rtt = tailscale_ping(target, peer_ip, ep_ip, &ep_port);
            if (rtt >= 0) {
                kprintf("pong from %s (%u.%u.%u.%u) via %u.%u.%u.%u:%u in %d ms\n",
                        target,
                        peer_ip[0], peer_ip[1], peer_ip[2], peer_ip[3],
                        ep_ip[0], ep_ip[1], ep_ip[2], ep_ip[3], ep_port, rtt);
            } else {
                kprintf("ping %s: timed out\n", target);
            }
            if (i < 2) task_sleep(300);
        }
        return;
    }

    kprintf("Usage:\n"
            "  tailscale                Open graphical Tailscale VPN manager (with iPhone QR login)\n"
            "  tailscale up [authkey]   Connect to Tailscale tailnet & display QR code\n"
            "  tailscale qr [url]       Display Tailscale login QR code on LCD & console\n"
            "  tailscale status         Show tailnet peers and tunnel status\n"
            "  tailscale ip             Print this Tab5's 100.x.y.z Tailscale IPv4\n"
            "  tailscale ping <peer>    Ping a Tailscale peer over Disco/WireGuard\n"
            "  tailscale down           Disconnect Tailscale tunnel\n");
}
