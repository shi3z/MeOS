/*
 * MeOS Web Browser (`web` / `browser`) for M5Stack Tab5
 *
 * Features:
 *  - HTTP/1.1 & HTTPS (TLS 1.3 RFC 8446 `TLS_CHACHA20_POLY1305_SHA256` and
 *    TLS 1.2 RFC 7905 `TLS_ECDHE_RSA/ECDSA_WITH_CHACHA20_POLY1305_SHA256` with X25519)
 *  - Works over both regular Wi-Fi and Tailscale mesh VPN (e.g. http://tsuginosuke:8000)
 *  - Automatic HTTP 301/302/303/307/308 redirect following & chunked transfer decoding
 *  - HTML5/HTML4 + UTF-8 Japanese (`efont` 16x16) layout engine in PSRAM
 *  - Keyboard-navigable hyperlinks (`[1]`, `[2]`, ... via Tab / Shift-Tab / Enter),
 *    page scrollback, history Back stack, DuckDuckGo / Wikipedia search, and Bookmarks
 */

#include "web_app.h"
#include "../tui/dqwin.h"
#include "../wifi/wifi_app.h"
#include <m5os/display.h>
#include <m5os/hal_time.h>
#include <m5os/kmalloc.h>
#include <m5os/kprintf.h>
#include <m5os/task.h>
#include <m5os/types.h>
#include <m5os/vfs.h>
#include <m5os/wifi.h>
#include <string.h>

#define WEB_SOCK_ID         2   /* Use TCP socket 2 so SSH (0..1) and Tailscale (3) are undisturbed */
#define WEB_CELL_PX         (8 * M5OS_FBCON_SCALE)
#define WEB_COLS            80
#define WEB_TOP_ROWS        2
#define WEB_BOT_ROWS        2
#define WEB_VIEW_ROWS       41  /* Rows 2..42 on 80x45 screen */
#define WEB_MAX_LINES       2400
#define WEB_MAX_LINKS       256
#define WEB_MAX_IMAGES      10
#define WEB_IMG_MAX_W       280
#define WEB_IMG_MAX_H       224
#define WEB_IMG_BUF_CAP     (140u * 1024u)
#define WEB_MAX_HIST        12
#define WEB_MAX_BOOKMARKS   16
#define WEB_HTML_BUF_CAP    (512u * 1024u)
#define WEB_BOOKMARKS_PATH  "/home/bookmarks.cfg"

/* =========================================================================
 * 1. Cryptographic Primitives for TLS 1.3 / TLS 1.2
 *    (SHA-256, HMAC-SHA256, HKDF-SHA256, X25519, ChaCha20-Poly1305)
 * ========================================================================= */

static uint32_t get_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put_le64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (i * 8));
}

static uint32_t rotr32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
static uint32_t rotl32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

static void web_rng_bytes(uint8_t *out, size_t len)
{
    static uint64_t s = 0x9E3779B97F4A7C15ULL;
    s ^= hal_time_now_us() * 0xBF58476D1CE4E5B9ULL;
    for (size_t i = 0; i < len; i++) {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        uint64_t z = s * 0x2545F4914F6CDD1DULL;
        out[i] = (uint8_t)(z >> 56);
    }
}

typedef struct {
    uint32_t h[8];
    uint64_t len;
    uint8_t  buf[64];
    size_t   buf_used;
} sha256_ctx_t;

static const uint32_t K256[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};

static void sha256_block(sha256_ctx_t *ctx, const uint8_t b[64])
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = get_be32(&b[i * 4]);
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = ctx->h[0], b0 = ctx->h[1], c = ctx->h[2], d = ctx->h[3];
    uint32_t e = ctx->h[4], f  = ctx->h[5], g = ctx->h[6], h = ctx->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + S1 + ch + K256[i] + w[i];
        uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b0) ^ (a & c) ^ (b0 & c);
        uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b0; b0 = a; a = t1 + t2;
    }
    ctx->h[0] += a; ctx->h[1] += b0; ctx->h[2] += c; ctx->h[3] += d;
    ctx->h[4] += e; ctx->h[5] += f;  ctx->h[6] += g; ctx->h[7] += h;
}

static void sha256_init(sha256_ctx_t *ctx)
{
    ctx->h[0] = 0x6a09e667u; ctx->h[1] = 0xbb67ae85u;
    ctx->h[2] = 0x3c6ef372u; ctx->h[3] = 0xa54ff53au;
    ctx->h[4] = 0x510e527fu; ctx->h[5] = 0x9b05688cu;
    ctx->h[6] = 0x1f83d9abu; ctx->h[7] = 0x5be0cd19u;
    ctx->len = 0;
    ctx->buf_used = 0;
}

static void sha256_update(sha256_ctx_t *ctx, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    ctx->len += len;
    while (len > 0) {
        size_t take = 64u - ctx->buf_used;
        if (take > len) take = len;
        memcpy(ctx->buf + ctx->buf_used, p, take);
        ctx->buf_used += take;
        p += take;
        len -= take;
        if (ctx->buf_used == 64u) {
            sha256_block(ctx, ctx->buf);
            ctx->buf_used = 0;
        }
    }
}

static void sha256_final(sha256_ctx_t *ctx, uint8_t out[32])
{
    sha256_ctx_t tmp = *ctx;
    uint64_t bits = tmp.len * 8ULL;
    uint8_t pad = 0x80u;
    sha256_update(&tmp, &pad, 1);
    uint8_t zero = 0;
    while (tmp.buf_used != 56u) sha256_update(&tmp, &zero, 1);
    uint8_t len_be[8];
    for (int i = 0; i < 8; i++) len_be[i] = (uint8_t)(bits >> ((7 - i) * 8));
    sha256_update(&tmp, len_be, 8);
    for (int i = 0; i < 8; i++) put_be32(&out[i * 4], tmp.h[i]);
}

static void hmac_sha256(const uint8_t *key, size_t key_len,
                        const uint8_t *data, size_t data_len, uint8_t out[32])
{
    uint8_t k_pad[64];
    memset(k_pad, 0, sizeof(k_pad));
    if (key_len > 64u) {
        sha256_ctx_t kt;
        sha256_init(&kt);
        sha256_update(&kt, key, key_len);
        sha256_final(&kt, k_pad);
    } else if (key_len > 0) {
        memcpy(k_pad, key, key_len);
    }

    uint8_t ipad[64], opad[64], inner[32];
    for (int i = 0; i < 64; i++) {
        ipad[i] = k_pad[i] ^ 0x36u;
        opad[i] = k_pad[i] ^ 0x5Cu;
    }

    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, ipad, 64);
    if (data_len > 0) sha256_update(&ctx, data, data_len);
    sha256_final(&ctx, inner);

    sha256_init(&ctx);
    sha256_update(&ctx, opad, 64);
    sha256_update(&ctx, inner, 32);
    sha256_final(&ctx, out);
}

static void hkdf_extract_sha256(const uint8_t *salt, size_t salt_len,
                                const uint8_t *ikm, size_t ikm_len, uint8_t prk[32])
{
    static const uint8_t zeros[32] = {0};
    if (!salt || salt_len == 0) {
        salt = zeros;
        salt_len = 32;
    }
    hmac_sha256(salt, salt_len, ikm, ikm_len, prk);
}

static void hkdf_expand_label(const uint8_t secret[32], const char *label,
                              const uint8_t *context, size_t context_len,
                              uint8_t *out, size_t out_len)
{
    /* HkdfLabel: uint16 length, opaque label<7..255> ("tls13 " + label), opaque context<0..255> */
    uint8_t info[128];
    size_t l_len = strlen(label);
    size_t full_l = 6u + l_len;
    size_t pos = 0;
    info[pos++] = (uint8_t)(out_len >> 8);
    info[pos++] = (uint8_t)(out_len & 0xFFu);
    info[pos++] = (uint8_t)full_l;
    memcpy(&info[pos], "tls13 ", 6); pos += 6;
    memcpy(&info[pos], label, l_len); pos += l_len;
    info[pos++] = (uint8_t)context_len;
    if (context_len > 0) {
        memcpy(&info[pos], context, context_len);
        pos += context_len;
    }
    info[pos++] = 0x01u; /* HKDF counter = 1 (out_len <= 32) */

    uint8_t digest[32];
    hmac_sha256(secret, 32, info, pos, digest);
    memcpy(out, digest, out_len);
}

/* TLS 1.2 PRF(secret, label, seed) with SHA-256 */
static void tls12_prf_sha256(const uint8_t *secret, size_t secret_len,
                             const char *label,
                             const uint8_t *seed, size_t seed_len,
                             uint8_t *out, size_t out_len)
{
    size_t l_len = strlen(label);
    uint8_t lseed[128];
    if (l_len + seed_len > sizeof(lseed)) return;
    memcpy(lseed, label, l_len);
    memcpy(lseed + l_len, seed, seed_len);
    size_t ls_len = l_len + seed_len;

    uint8_t a[32];
    hmac_sha256(secret, secret_len, lseed, ls_len, a); /* A(1) */

    size_t produced = 0;
    while (produced < out_len) {
        uint8_t buf[32 + sizeof(lseed)];
        memcpy(buf, a, 32);
        memcpy(buf + 32, lseed, ls_len);
        uint8_t p[32];
        hmac_sha256(secret, secret_len, buf, 32 + ls_len, p);
        size_t take = (out_len - produced < 32u) ? (out_len - produced) : 32u;
        memcpy(out + produced, p, take);
        produced += take;
        hmac_sha256(secret, secret_len, a, 32, a); /* A(i+1) */
    }
}

/* Curve25519 (X25519) */
typedef int64_t fe25519[16];

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

static void fe_carry(fe25519 o)
{
    for (int i = 0; i < 16; i++) {
        o[i] += (1LL << 16);
        int64_t c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c << 16;
    }
}

static void fe_cswap(fe25519 p, fe25519 q, int64_t b)
{
    int64_t mask = ~(b - 1);
    for (int i = 0; i < 16; i++) {
        int64_t t = mask & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

static void fe_mul(fe25519 out, const fe25519 a, const fe25519 b)
{
    int64_t t[31] = {0};
    for (int i = 0; i < 16; i++) {
        for (int j = 0; j < 16; j++) t[i + j] += a[i] * b[j];
    }
    for (int i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; i++) out[i] = t[i];
    fe_carry(out);
    fe_carry(out);
}

static void x25519_scalarmult(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32])
{
    uint8_t z[32];
    memcpy(z, scalar, 32);
    z[0]  &= 248u;
    z[31] = (z[31] & 127u) | 64u;

    fe25519 x, a = {0}, b = {0}, c = {0}, d = {0}, e, f;
    for (int i = 0; i < 16; i++) x[i] = (int64_t)point[2 * i] + ((int64_t)point[2 * i + 1] << 8);
    x[15] &= 0x7FFF;
    a[0] = 1; d[0] = 1;
    fe_copy(b, x);

    for (int i = 254; i >= 0; i--) {
        int64_t r = (z[i >> 3] >> (i & 7)) & 1;
        fe_cswap(a, b, r);
        fe_cswap(c, d, r);
        fe_add(e, a, c);
        fe_sub(a, a, c);
        fe_add(c, b, d);
        fe_sub(b, b, d);
        fe_mul(d, e, e);
        fe_mul(f, a, a);
        fe_mul(a, c, a);
        fe_mul(c, b, e);
        fe_add(e, a, c);
        fe_sub(a, a, c);
        fe_mul(b, a, a);
        fe_sub(c, d, f);
        static const fe25519 k121665 = { 0xDB41, 1, 0,0,0,0,0,0,0,0,0,0,0,0,0,0 };
        fe_mul(a, c, k121665);
        fe_add(a, a, d);
        fe_mul(c, c, a);
        fe_mul(a, d, f);
        fe_mul(d, b, x);
        fe_mul(b, e, e);
        fe_cswap(a, b, r);
        fe_cswap(c, d, r);
    }

    /* Invert c: c^(2^255 - 21) */
    fe25519 inv;
    fe_copy(inv, c);
    for (int i = 253; i >= 0; i--) {
        fe_mul(inv, inv, inv);
        if (i != 2 && i != 4) fe_mul(inv, inv, c);
    }
    fe_mul(a, a, inv);
    fe_carry(a);
    fe_carry(a);
    fe_carry(a);

    fe25519 m;
    for (int j = 0; j < 2; j++) {
        m[0] = a[0] - 0xFFED;
        for (int i = 1; i < 15; i++) {
            m[i] = a[i] - 0xFFFF - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xFFFF;
        }
        m[15] = a[15] - 0x7FFF - ((m[14] >> 16) & 1);
        int64_t carry = (m[15] >> 16) & 1;
        m[14] &= 0xFFFF;
        fe_cswap(a, m, 1 - carry);
    }
    for (int i = 0; i < 16; i++) {
        out[2 * i]     = (uint8_t)(a[i] & 0xFF);
        out[2 * i + 1] = (uint8_t)((a[i] >> 8) & 0xFF);
    }
}

/* ChaCha20-Poly1305 (RFC 8439) */
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

static void chacha20_xor(uint8_t *out, const uint8_t *in, size_t len,
                         const uint8_t key[32], uint32_t counter, const uint8_t nonce[12])
{
    uint8_t blk[64];
    size_t pos = 0;
    while (pos < len) {
        chacha20_block(blk, key, counter++, nonce);
        size_t take = (len - pos > 64u) ? 64u : (len - pos);
        for (size_t i = 0; i < take; i++) out[pos + i] = in[pos + i] ^ blk[i];
        pos += take;
    }
}

typedef struct {
    uint32_t r[5];
    uint32_t pad[4];
    uint32_t h[5];
    uint8_t  buf[16];
    size_t   buf_used;
} poly1305_ctx_t;

static void poly1305_init(poly1305_ctx_t *ctx, const uint8_t key[32])
{
    ctx->r[0] = (get_le32(&key[0]))                     & 0x3ffffffu;
    ctx->r[1] = (get_le32(&key[3])  >> 2)               & 0x3ffff03u;
    ctx->r[2] = (get_le32(&key[6])  >> 4)               & 0x3ffc0ffu;
    ctx->r[3] = (get_le32(&key[9])  >> 6)               & 0x3f03fffu;
    ctx->r[4] = (get_le32(&key[12]) >> 8)               & 0x00fffffu;
    ctx->pad[0] = get_le32(&key[16]);
    ctx->pad[1] = get_le32(&key[20]);
    ctx->pad[2] = get_le32(&key[24]);
    ctx->pad[3] = get_le32(&key[28]);
    memset(ctx->h, 0, sizeof(ctx->h));
    ctx->buf_used = 0;
}

static void poly1305_blocks(poly1305_ctx_t *ctx, const uint8_t *m, size_t bytes, uint32_t hibit)
{
    uint32_t r0 = ctx->r[0], r1 = ctx->r[1], r2 = ctx->r[2], r3 = ctx->r[3], r4 = ctx->r[4];
    uint32_t s1 = r1 * 5u, s2 = r2 * 5u, s3 = r3 * 5u, s4 = r4 * 5u;
    uint32_t h0 = ctx->h[0], h1 = ctx->h[1], h2 = ctx->h[2], h3 = ctx->h[3], h4 = ctx->h[4];

    while (bytes >= 16u) {
        h0 += (get_le32(&m[0]))       & 0x3ffffffu;
        h1 += (get_le32(&m[3])  >> 2) & 0x3ffffffu;
        h2 += (get_le32(&m[6])  >> 4) & 0x3ffffffu;
        h3 += (get_le32(&m[9])  >> 6) & 0x3ffffffu;
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
        c = h0 >> 26;             h0 &= 0x3ffffffu;               h1 += c;

        m += 16;
        bytes -= 16;
    }
    ctx->h[0] = h0; ctx->h[1] = h1; ctx->h[2] = h2; ctx->h[3] = h3; ctx->h[4] = h4;
}

static void poly1305_update(poly1305_ctx_t *ctx, const uint8_t *m, size_t bytes)
{
    if (ctx->buf_used > 0) {
        size_t want = 16u - ctx->buf_used;
        if (want > bytes) want = bytes;
        memcpy(ctx->buf + ctx->buf_used, m, want);
        m += want;
        bytes -= want;
        ctx->buf_used += want;
        if (ctx->buf_used < 16u) return;
        poly1305_blocks(ctx, ctx->buf, 16, 1);
        ctx->buf_used = 0;
    }
    if (bytes >= 16u) {
        size_t want = bytes & ~(size_t)15u;
        poly1305_blocks(ctx, m, want, 1);
        m += want;
        bytes -= want;
    }
    if (bytes > 0) {
        memcpy(ctx->buf, m, bytes);
        ctx->buf_used = bytes;
    }
}

static void poly1305_final(poly1305_ctx_t *ctx, uint8_t mac[16])
{
    if (ctx->buf_used > 0) {
        size_t i = ctx->buf_used;
        ctx->buf[i++] = 1;
        while (i < 16u) ctx->buf[i++] = 0;
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

    h0 = (h0)       | (h1 << 26);
    h1 = (h1 >> 6)  | (h2 << 20);
    h2 = (h2 >> 12) | (h3 << 14);
    h3 = (h3 >> 18) | (h4 << 8);

    uint64_t f;
    f = (uint64_t)h0 + ctx->pad[0];              h0 = (uint32_t)f;
    f = (uint64_t)h1 + ctx->pad[1] + (f >> 32);  h1 = (uint32_t)f;
    f = (uint64_t)h2 + ctx->pad[2] + (f >> 32);  h2 = (uint32_t)f;
    f = (uint64_t)h3 + ctx->pad[3] + (f >> 32);  h3 = (uint32_t)f;

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
    if (aad_len > 0) {
        poly1305_update(&pctx, aad, aad_len);
        if (aad_len & 15u) poly1305_update(&pctx, zeros, 16u - (aad_len & 15u));
    }
    if (ct_len > 0) {
        poly1305_update(&pctx, ct, ct_len);
        if (ct_len & 15u) poly1305_update(&pctx, zeros, 16u - (ct_len & 15u));
    }
    uint8_t lens[16];
    put_le64(&lens[0], (uint64_t)aad_len);
    put_le64(&lens[8], (uint64_t)ct_len);
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
    uint8_t block0[64], expected[16];
    chacha20_block(block0, key, 0, nonce);
    chachapoly_compute_tag(expected, block0, aad, aad_len, ct_and_tag, ct_len);
    uint8_t diff = 0;
    for (int i = 0; i < 16; i++) diff |= expected[i] ^ ct_and_tag[ct_len + i];
    if (diff != 0) return false;
    if (ct_len > 0 && out) chacha20_xor(out, ct_and_tag, ct_len, key, 1, nonce);
    return true;
}

static void tls_build_nonce(uint8_t nonce[12], const uint8_t iv[12], uint64_t seq)
{
    memcpy(nonce, iv, 12);
    for (int i = 0; i < 8; i++) {
        nonce[11 - i] ^= (uint8_t)((seq >> (i * 8)) & 0xFFu);
    }
}

/* =========================================================================
 * 2. TLS 1.3 & TLS 1.2 Client Engine
 * ========================================================================= */

#define TLS_REC_MAX 16640u

typedef struct {
    int      sock;
    bool     is_tls;
    bool     is_tls13;
    uint16_t cipher_suite;
    uint8_t  c_key[32];
    uint8_t  c_iv[12];
    uint8_t  s_key[32];
    uint8_t  s_iv[12];
    uint64_t c_seq;
    uint64_t s_seq;
    uint8_t *rec_buf;     /* TLS_REC_MAX buffer in PSRAM */
    uint8_t *plain_buf;   /* TLS_REC_MAX buffer in PSRAM */
    size_t   plain_len;
    size_t   plain_pos;
} web_conn_t;

static bool tcp_recv_exact(int sock, uint8_t *out, size_t n, uint32_t timeout_ms)
{
    size_t got = 0;
    uint64_t deadline = hal_time_now_us() + (uint64_t)timeout_ms * 1000ULL;
    while (got < n && hal_time_now_us() < deadline) {
        int r = wifi_tcp_recv_sock(sock, out + got, n - got, 250);
        if (r < 0) return false;
        if (r == 0 && !wifi_tcp_is_connected_sock(sock)) return false;
        got += (size_t)r;
    }
    return got == n;
}

static bool tls_send_record_enc(web_conn_t *c, uint8_t content_type, const uint8_t *pt, size_t pt_len)
{
    uint8_t nonce[12], aad[5], hdr[5];
    tls_build_nonce(nonce, c->c_iv, c->c_seq);

    if (c->is_tls13) {
        /* TLS 1.3: inner plaintext = pt || content_type; outer type = 23 */
        memcpy(c->plain_buf, pt, pt_len);
        c->plain_buf[pt_len] = content_type;
        size_t inner_len = pt_len + 1u;
        size_t ct_len = inner_len + 16u;
        aad[0] = 23u; aad[1] = 0x03u; aad[2] = 0x03u;
        aad[3] = (uint8_t)(ct_len >> 8); aad[4] = (uint8_t)(ct_len & 0xFFu);
        memcpy(c->rec_buf, aad, 5);
        chachapoly_seal(c->rec_buf + 5, c->c_key, nonce, c->plain_buf, inner_len, aad, 5);
        c->c_seq++;
        return wifi_tcp_send_sock(c->sock, c->rec_buf, 5u + ct_len, 5000) == (int)(5u + ct_len);
    } else {
        /* TLS 1.2 RFC 7905 ChaCha20-Poly1305: AAD = seq_be64(8) || type(1) || version(2) || pt_len(2) */
        uint8_t aad12[13];
        for (int i = 0; i < 8; i++) aad12[i] = (uint8_t)((c->c_seq >> ((7 - i) * 8)) & 0xFFu);
        aad12[8]  = content_type;
        aad12[9]  = 0x03u;
        aad12[10] = 0x03u;
        aad12[11] = (uint8_t)(pt_len >> 8);
        aad12[12] = (uint8_t)(pt_len & 0xFFu);

        size_t ct_len = pt_len + 16u;
        hdr[0] = content_type; hdr[1] = 0x03u; hdr[2] = 0x03u;
        hdr[3] = (uint8_t)(ct_len >> 8); hdr[4] = (uint8_t)(ct_len & 0xFFu);
        memcpy(c->rec_buf, hdr, 5);
        chachapoly_seal(c->rec_buf + 5, c->c_key, nonce, pt, pt_len, aad12, 13);
        c->c_seq++;
        return wifi_tcp_send_sock(c->sock, c->rec_buf, 5u + ct_len, 5000) == (int)(5u + ct_len);
    }
}

static bool tls_recv_record_enc(web_conn_t *c, uint8_t *out_type, size_t *out_len, uint32_t timeout_ms)
{
    for (;;) {
        uint8_t hdr[5];
        if (!tcp_recv_exact(c->sock, hdr, 5, timeout_ms)) return false;
        uint8_t rtype = hdr[0];
        uint16_t rlen = ((uint16_t)hdr[3] << 8) | hdr[4];
        if (rlen > TLS_REC_MAX) return false;
        if (!tcp_recv_exact(c->sock, c->rec_buf, rlen, timeout_ms)) return false;

        /* Skip unencrypted ChangeCipherSpec (type 20) in TLS 1.3 */
        if (rtype == 20u) continue;
        /* Unencrypted Alert */
        if (rtype == 21u && rlen == 2u) return false;

        uint8_t nonce[12];
        tls_build_nonce(nonce, c->s_iv, c->s_seq);

        if (c->is_tls13) {
            if (rtype != 23u || rlen < 17u) return false;
            if (!chachapoly_open(c->plain_buf, c->s_key, nonce, c->rec_buf, rlen, hdr, 5)) {
                return false;
            }
            c->s_seq++;
            size_t plen = (size_t)rlen - 16u;
            while (plen > 0 && c->plain_buf[plen - 1] == 0) plen--;
            if (plen == 0) return false;
            *out_type = c->plain_buf[plen - 1];
            *out_len  = plen - 1u;
            return true;
        } else {
            if (rlen < 16u) return false;
            size_t plen = (size_t)rlen - 16u;
            uint8_t aad12[13];
            for (int i = 0; i < 8; i++) aad12[i] = (uint8_t)((c->s_seq >> ((7 - i) * 8)) & 0xFFu);
            aad12[8]  = rtype;
            aad12[9]  = hdr[1];
            aad12[10] = hdr[2];
            aad12[11] = (uint8_t)(plen >> 8);
            aad12[12] = (uint8_t)(plen & 0xFFu);
            if (!chachapoly_open(c->plain_buf, c->s_key, nonce, c->rec_buf, rlen, aad12, 13)) {
                return false;
            }
            c->s_seq++;
            *out_type = rtype;
            *out_len  = plen;
            return true;
        }
    }
}

static bool tls_handshake(web_conn_t *c, const char *host)
{
    static const uint8_t basepoint9[32] = { 9 };
    uint8_t client_priv[32], client_pub[32], client_random[32], session_id[32];
    web_rng_bytes(client_priv, 32);
    client_priv[0]  &= 248u;
    client_priv[31] = (client_priv[31] & 127u) | 64u;
    x25519_scalarmult(client_pub, client_priv, basepoint9);
    web_rng_bytes(client_random, 32);
    web_rng_bytes(session_id, 32);

    /* Build ClientHello offering TLS 1.3 (0x1303) & TLS 1.2 (0xCCA8, 0xCCA9) with X25519 */
    uint8_t *ch = c->rec_buf;
    size_t p = 9; /* Reserve 5B record header + 4B handshake header */
    ch[p++] = 0x03u; ch[p++] = 0x03u; /* Legacy version TLS 1.2 */
    memcpy(&ch[p], client_random, 32); p += 32;
    ch[p++] = 32u;
    memcpy(&ch[p], session_id, 32); p += 32;

    /* Cipher Suites (6 bytes): TLS_CHACHA20_POLY1305_SHA256, ECDHE_ECDSA_CHACHA20, ECDHE_RSA_CHACHA20 */
    ch[p++] = 0x00u; ch[p++] = 0x06u;
    ch[p++] = 0x13u; ch[p++] = 0x03u;
    ch[p++] = 0xCCu; ch[p++] = 0xA9u;
    ch[p++] = 0xCCu; ch[p++] = 0xA8u;

    /* Compression methods: null */
    ch[p++] = 0x01u; ch[p++] = 0x00u;

    size_t ext_len_pos = p;
    p += 2;

    /* Extension 0x0000: server_name (SNI) */
    size_t hlen = strlen(host);
    ch[p++] = 0x00u; ch[p++] = 0x00u;
    ch[p++] = (uint8_t)((hlen + 5u) >> 8); ch[p++] = (uint8_t)((hlen + 5u) & 0xFFu);
    ch[p++] = (uint8_t)((hlen + 3u) >> 8); ch[p++] = (uint8_t)((hlen + 3u) & 0xFFu);
    ch[p++] = 0x00u;
    ch[p++] = (uint8_t)(hlen >> 8); ch[p++] = (uint8_t)(hlen & 0xFFu);
    memcpy(&ch[p], host, hlen); p += hlen;

    /* Extension 0x000A: supported_groups (x25519 = 0x001D) */
    static const uint8_t ext_groups[] = { 0x00,0x0A, 0x00,0x04, 0x00,0x02, 0x00,0x1D };
    memcpy(&ch[p], ext_groups, sizeof(ext_groups)); p += sizeof(ext_groups);

    /* Extension 0x000B: ec_point_formats (uncompressed) */
    static const uint8_t ext_ecpf[] = { 0x00,0x0B, 0x00,0x02, 0x01, 0x00 };
    memcpy(&ch[p], ext_ecpf, sizeof(ext_ecpf)); p += sizeof(ext_ecpf);

    /* Extension 0x000D: signature_algorithms */
    static const uint8_t ext_sigalgs[] = {
        0x00,0x0D, 0x00,0x12, 0x00,0x10,
        0x04,0x03, 0x05,0x03, 0x06,0x03,
        0x08,0x04, 0x08,0x05, 0x08,0x06,
        0x08,0x07, 0x04,0x01
    };
    memcpy(&ch[p], ext_sigalgs, sizeof(ext_sigalgs)); p += sizeof(ext_sigalgs);

    /* Extension 0x002B: supported_versions (TLS 1.3 = 0x0304, TLS 1.2 = 0x0303) */
    static const uint8_t ext_vers[] = { 0x00,0x2B, 0x00,0x05, 0x04, 0x03,0x04, 0x03,0x03 };
    memcpy(&ch[p], ext_vers, sizeof(ext_vers)); p += sizeof(ext_vers);

    /* Extension 0x002D: psk_key_exchange_modes (psk_dhe_ke = 1) */
    static const uint8_t ext_psk_mode[] = { 0x00,0x2D, 0x00,0x02, 0x01, 0x01 };
    memcpy(&ch[p], ext_psk_mode, sizeof(ext_psk_mode)); p += sizeof(ext_psk_mode);

    /* Extension 0x0033: key_share (x25519 = 0x001D, 32B client_pub) */
    ch[p++] = 0x00u; ch[p++] = 0x33u;
    ch[p++] = 0x00u; ch[p++] = 38u;
    ch[p++] = 0x00u; ch[p++] = 36u;
    ch[p++] = 0x00u; ch[p++] = 0x1Du;
    ch[p++] = 0x00u; ch[p++] = 32u;
    memcpy(&ch[p], client_pub, 32); p += 32;

    /* Fill lengths */
    size_t ext_total = p - (ext_len_pos + 2u);
    ch[ext_len_pos]     = (uint8_t)(ext_total >> 8);
    ch[ext_len_pos + 1] = (uint8_t)(ext_total & 0xFFu);

    size_t hs_body_len = p - 9u;
    ch[5] = 1u; /* ClientHello */
    ch[6] = 0u;
    ch[7] = (uint8_t)(hs_body_len >> 8);
    ch[8] = (uint8_t)(hs_body_len & 0xFFu);

    size_t rec_len = p - 5u;
    ch[0] = 22u; ch[1] = 0x03u; ch[2] = 0x01u;
    ch[3] = (uint8_t)(rec_len >> 8);
    ch[4] = (uint8_t)(rec_len & 0xFFu);

    sha256_ctx_t transcript;
    sha256_init(&transcript);
    sha256_update(&transcript, ch + 5, rec_len);

    if (wifi_tcp_send_sock(c->sock, ch, p, 5000) != (int)p) return false;

    /* Read ServerHello record */
    uint8_t sh_hdr[5];
    if (!tcp_recv_exact(c->sock, sh_hdr, 5, 6000)) return false;
    uint16_t sh_rlen = ((uint16_t)sh_hdr[3] << 8) | sh_hdr[4];
    if (sh_hdr[0] != 22u || sh_rlen < 42u || sh_rlen > TLS_REC_MAX) return false;
    if (!tcp_recv_exact(c->sock, c->rec_buf, sh_rlen, 6000)) return false;

    const uint8_t *sh = c->rec_buf;
    if (sh[0] != 2u) return false; /* Must be ServerHello */
    uint32_t sh_body_len = ((uint32_t)sh[1] << 16) | ((uint32_t)sh[2] << 8) | sh[3];
    if (4u + sh_body_len > sh_rlen) return false;

    sha256_update(&transcript, sh, 4u + sh_body_len);

    uint8_t server_random[32];
    memcpy(server_random, &sh[6], 32);
    size_t sp = 38u;
    if (sp >= 4u + sh_body_len) return false;
    uint8_t sid_len = sh[sp++];
    sp += sid_len;
    if (sp + 3u > 4u + sh_body_len) return false;
    c->cipher_suite = ((uint16_t)sh[sp] << 8) | sh[sp + 1];
    sp += 3u; /* cipher_suite(2) + compression(1) */

    bool is_tls13 = (c->cipher_suite == 0x1303u);
    uint8_t server_pub[32];
    bool got_server_pub = false;

    if (sp + 2u <= 4u + sh_body_len) {
        uint16_t elen = ((uint16_t)sh[sp] << 8) | sh[sp + 1];
        sp += 2u;
        size_t eend = sp + elen;
        if (eend > 4u + sh_body_len) eend = 4u + sh_body_len;
        while (sp + 4u <= eend) {
            uint16_t etype = ((uint16_t)sh[sp] << 8) | sh[sp + 1];
            uint16_t esz   = ((uint16_t)sh[sp + 2] << 8) | sh[sp + 3];
            sp += 4u;
            if (sp + esz > eend) break;
            if (etype == 0x002Bu && esz >= 2u) {
                uint16_t ver = ((uint16_t)sh[sp] << 8) | sh[sp + 1];
                if (ver == 0x0304u) is_tls13 = true;
            } else if (etype == 0x0033u && esz >= 36u) {
                uint16_t grp = ((uint16_t)sh[sp] << 8) | sh[sp + 1];
                uint16_t ksz = ((uint16_t)sh[sp + 2] << 8) | sh[sp + 3];
                if (grp == 0x001Du && ksz == 32u) {
                    memcpy(server_pub, &sh[sp + 4], 32);
                    got_server_pub = true;
                }
            }
            sp += esz;
        }
    }

    c->is_tls13 = is_tls13;

    if (is_tls13) {
        if (!got_server_pub) return false;
        uint8_t shared[32], zero32[32], empty_hash[32];
        memset(zero32, 0, 32);
        x25519_scalarmult(shared, client_priv, server_pub);

        sha256_ctx_t ectx;
        sha256_init(&ectx);
        sha256_final(&ectx, empty_hash);

        uint8_t early_secret[32], derived0[32], hs_secret[32], h_hello[32];
        hkdf_extract_sha256(zero32, 32, zero32, 32, early_secret);
        hkdf_expand_label(early_secret, "derived", empty_hash, 32, derived0, 32);
        hkdf_extract_sha256(derived0, 32, shared, 32, hs_secret);

        sha256_final(&transcript, h_hello);

        uint8_t c_hs_tr[32], s_hs_tr[32];
        hkdf_expand_label(hs_secret, "c hs traffic", h_hello, 32, c_hs_tr, 32);
        hkdf_expand_label(hs_secret, "s hs traffic", h_hello, 32, s_hs_tr, 32);

        hkdf_expand_label(c_hs_tr, "key", NULL, 0, c->c_key, 32);
        hkdf_expand_label(c_hs_tr, "iv",  NULL, 0, c->c_iv,  12);
        hkdf_expand_label(s_hs_tr, "key", NULL, 0, c->s_key, 32);
        hkdf_expand_label(s_hs_tr, "iv",  NULL, 0, c->s_iv,  12);
        c->c_seq = 0;
        c->s_seq = 0;

        /* Read encrypted server handshake messages until Server Finished (type 20) */
        bool got_server_fin = false;
        uint32_t hs_rem_bytes = 0;
        uint8_t  hs_cur_type  = 0;
        uint8_t  hs_hdr_buf[4];
        size_t   hs_hdr_got   = 0;

        while (!got_server_fin) {
            uint8_t rtype = 0;
            size_t rlen = 0;
            if (!tls_recv_record_enc(c, &rtype, &rlen, 6000)) return false;
            if (rtype != 22u) continue;

            sha256_update(&transcript, c->plain_buf, rlen);

            size_t pos = 0;
            while (pos < rlen) {
                if (hs_rem_bytes == 0) {
                    while (hs_hdr_got < 4u && pos < rlen) {
                        hs_hdr_buf[hs_hdr_got++] = c->plain_buf[pos++];
                    }
                    if (hs_hdr_got < 4u) break;
                    hs_cur_type  = hs_hdr_buf[0];
                    hs_rem_bytes = ((uint32_t)hs_hdr_buf[1] << 16) |
                                   ((uint32_t)hs_hdr_buf[2] << 8)  |
                                   (uint32_t)hs_hdr_buf[3];
                    hs_hdr_got = 0;
                    if (hs_rem_bytes == 0 && hs_cur_type == 20u) {
                        got_server_fin = true;
                    }
                } else {
                    size_t take = rlen - pos;
                    if (take > hs_rem_bytes) take = hs_rem_bytes;
                    pos += take;
                    hs_rem_bytes -= (uint32_t)take;
                    if (hs_rem_bytes == 0 && hs_cur_type == 20u) {
                        got_server_fin = true;
                    }
                }
            }
        }

        uint8_t h_srv_fin[32], derived1[32], master_sec[32];
        sha256_final(&transcript, h_srv_fin);

        /* Send Client ChangeCipherSpec (compatibility) + Client Finished */
        static const uint8_t ccs_rec[6] = { 20, 0x03, 0x03, 0x00, 0x01, 0x01 };
        wifi_tcp_send_sock(c->sock, ccs_rec, 6, 3000);

        uint8_t fin_key[32], fin_msg[36];
        hkdf_expand_label(c_hs_tr, "finished", NULL, 0, fin_key, 32);
        fin_msg[0] = 20u; fin_msg[1] = 0u; fin_msg[2] = 0u; fin_msg[3] = 32u;
        hmac_sha256(fin_key, 32, h_srv_fin, 32, &fin_msg[4]);
        if (!tls_send_record_enc(c, 22u, fin_msg, 36)) return false;

        /* Derive Application Traffic Keys */
        hkdf_expand_label(hs_secret, "derived", empty_hash, 32, derived1, 32);
        hkdf_extract_sha256(derived1, 32, zero32, 32, master_sec);

        uint8_t c_ap_tr[32], s_ap_tr[32];
        hkdf_expand_label(master_sec, "c ap traffic", h_srv_fin, 32, c_ap_tr, 32);
        hkdf_expand_label(master_sec, "s ap traffic", h_srv_fin, 32, s_ap_tr, 32);

        hkdf_expand_label(c_ap_tr, "key", NULL, 0, c->c_key, 32);
        hkdf_expand_label(c_ap_tr, "iv",  NULL, 0, c->c_iv,  12);
        hkdf_expand_label(s_ap_tr, "key", NULL, 0, c->s_key, 32);
        hkdf_expand_label(s_ap_tr, "iv",  NULL, 0, c->s_iv,  12);
        c->c_seq = 0;
        c->s_seq = 0;
        return true;
    }

    /* TLS 1.2 Handshake (0xCCA8 / 0xCCA9 with X25519 ECDHE) */
    size_t rem_in_first = sh_rlen - (4u + sh_body_len);
    size_t cur_off = 4u + sh_body_len;
    bool got_shd = false;

    while (!got_shd) {
        const uint8_t *buf = NULL;
        size_t blen = 0;
        if (rem_in_first > 0) {
            buf  = c->rec_buf + cur_off;
            blen = rem_in_first;
            rem_in_first = 0;
        } else {
            uint8_t hdr[5];
            if (!tcp_recv_exact(c->sock, hdr, 5, 6000)) return false;
            uint16_t rlen = ((uint16_t)hdr[3] << 8) | hdr[4];
            if (hdr[0] != 22u || rlen > TLS_REC_MAX) return false;
            if (!tcp_recv_exact(c->sock, c->rec_buf, rlen, 6000)) return false;
            buf  = c->rec_buf;
            blen = rlen;
        }
        sha256_update(&transcript, buf, blen);
        size_t pos = 0;
        while (pos + 4u <= blen) {
            uint8_t htype = buf[pos];
            uint32_t hlen2 = ((uint32_t)buf[pos + 1] << 16) | ((uint32_t)buf[pos + 2] << 8) | buf[pos + 3];
            if (pos + 4u + hlen2 > blen) break;
            const uint8_t *hbody = &buf[pos + 4u];
            if (htype == 12u && hlen2 >= 36u) {
                /* ServerKeyExchange: curve_type=3, named_curve=0x001D, pub_len=32 */
                if (hbody[0] == 3u && hbody[1] == 0x00u && hbody[2] == 0x1Du && hbody[3] == 32u) {
                    memcpy(server_pub, &hbody[4], 32);
                    got_server_pub = true;
                }
            } else if (htype == 14u) {
                got_shd = true;
            }
            pos += 4u + hlen2;
        }
    }

    if (!got_server_pub) return false;

    /* Send ClientKeyExchange (type 16, length 33: 0x20 || client_pub[32]) */
    uint8_t cke_rec[5 + 37];
    cke_rec[0] = 22u; cke_rec[1] = 0x03u; cke_rec[2] = 0x03u;
    cke_rec[3] = 0u;  cke_rec[4] = 37u;
    cke_rec[5] = 16u; cke_rec[6] = 0u; cke_rec[7] = 0u; cke_rec[8] = 33u;
    cke_rec[9] = 32u;
    memcpy(&cke_rec[10], client_pub, 32);
    sha256_update(&transcript, &cke_rec[5], 37);
    if (wifi_tcp_send_sock(c->sock, cke_rec, sizeof(cke_rec), 5000) != (int)sizeof(cke_rec)) return false;

    /* Derive TLS 1.2 master_secret and key_block */
    uint8_t pre_master[32], master_sec[48], seed64[64], key_block[88];
    x25519_scalarmult(pre_master, client_priv, server_pub);
    memcpy(seed64, client_random, 32);
    memcpy(seed64 + 32, server_random, 32);
    tls12_prf_sha256(pre_master, 32, "master secret", seed64, 64, master_sec, 48);

    memcpy(seed64, server_random, 32);
    memcpy(seed64 + 32, client_random, 32);
    tls12_prf_sha256(master_sec, 48, "key expansion", seed64, 64, key_block, 88);
    memcpy(c->c_key, &key_block[0],  32);
    memcpy(c->s_key, &key_block[32], 32);
    memcpy(c->c_iv,  &key_block[64], 12);
    memcpy(c->s_iv,  &key_block[76], 12);
    c->c_seq = 0;
    c->s_seq = 0;

    /* Send ChangeCipherSpec + Client Finished */
    static const uint8_t ccs_rec[6] = { 20, 0x03, 0x03, 0x00, 0x01, 0x01 };
    if (wifi_tcp_send_sock(c->sock, ccs_rec, 6, 3000) != 6) return false;

    uint8_t h_fin[32], fin_msg[16];
    sha256_final(&transcript, h_fin);
    fin_msg[0] = 20u; fin_msg[1] = 0u; fin_msg[2] = 0u; fin_msg[3] = 12u;
    tls12_prf_sha256(master_sec, 48, "client finished", h_fin, 32, &fin_msg[4], 12);
    if (!tls_send_record_enc(c, 22u, fin_msg, 16)) return false;

    /* Receive Server Finished */
    uint8_t rtype = 0;
    size_t rlen = 0;
    if (!tls_recv_record_enc(c, &rtype, &rlen, 6000)) return false;
    return rtype == 22u;
}

static bool web_conn_open(web_conn_t *c, const char *host, uint16_t port, bool is_tls)
{
    memset(c, 0, sizeof(*c));
    c->sock   = WEB_SOCK_ID;
    c->is_tls = is_tls;

    c->rec_buf   = (uint8_t *)kmalloc_psram(TLS_REC_MAX);
    c->plain_buf = (uint8_t *)kmalloc_psram(TLS_REC_MAX);
    if (!c->rec_buf || !c->plain_buf) {
        if (c->rec_buf) kfree(c->rec_buf);
        if (c->plain_buf) kfree(c->plain_buf);
        return false;
    }

    uint8_t ip[4] = {0};
    if (!wifi_resolve_host(host, ip)) {
        kfree(c->rec_buf);
        kfree(c->plain_buf);
        return false;
    }

    if (!wifi_tcp_connect_sock(c->sock, ip, port, 7000)) {
        kfree(c->rec_buf);
        kfree(c->plain_buf);
        return false;
    }

    if (is_tls) {
        if (!tls_handshake(c, host)) {
            wifi_tcp_close_sock(c->sock);
            kfree(c->rec_buf);
            kfree(c->plain_buf);
            return false;
        }
    }
    return true;
}

static void web_conn_close(web_conn_t *c)
{
    wifi_tcp_close_sock(c->sock);
    if (c->rec_buf)   { kfree(c->rec_buf);   c->rec_buf = NULL; }
    if (c->plain_buf) { kfree(c->plain_buf); c->plain_buf = NULL; }
}

static bool web_conn_write(web_conn_t *c, const void *data, size_t len)
{
    if (!c->is_tls) {
        return wifi_tcp_send_sock(c->sock, data, len, 5000) == (int)len;
    }
    const uint8_t *p = (const uint8_t *)data;
    while (len > 0) {
        size_t chunk = (len > 4096u) ? 4096u : len;
        if (!tls_send_record_enc(c, 23u, p, chunk)) return false;
        p += chunk;
        len -= chunk;
    }
    return true;
}

static int web_conn_read(web_conn_t *c, uint8_t *out, size_t max_len, uint32_t timeout_ms)
{
    if (!c->is_tls) {
        int r = wifi_tcp_recv_sock(c->sock, out, max_len, timeout_ms);
        if (r == 0 && !wifi_tcp_is_connected_sock(c->sock)) return -1;
        return r;
    }

    if (c->plain_pos < c->plain_len) {
        size_t avail = c->plain_len - c->plain_pos;
        if (avail > max_len) avail = max_len;
        memcpy(out, c->plain_buf + c->plain_pos, avail);
        c->plain_pos += avail;
        return (int)avail;
    }

    for (;;) {
        uint8_t rtype = 0;
        size_t rlen = 0;
        if (!tls_recv_record_enc(c, &rtype, &rlen, timeout_ms)) return -1;
        if (rtype == 21u) return -1; /* Alert / close_notify */
        if (rtype == 22u) continue;  /* Ignore TLS 1.3 NewSessionTicket */
        if (rtype == 23u && rlen > 0) {
            c->plain_len = rlen;
            c->plain_pos = 0;
            size_t avail = (rlen > max_len) ? max_len : rlen;
            memcpy(out, c->plain_buf, avail);
            c->plain_pos = avail;
            return (int)avail;
        }
    }
}

/* =========================================================================
 * 3. URL Parsing, Relative URL Resolution & HTTP/HTTPS Fetcher
 * ========================================================================= */

static char to_lower_ascii(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static bool str_istarts_with(const char *s, const char *prefix)
{
    while (*prefix) {
        if (to_lower_ascii(*s++) != to_lower_ascii(*prefix++)) return false;
    }
    return true;
}

static const char *str_ifind(const char *hay, const char *needle)
{
    if (!*needle) return hay;
    size_t nlen = strlen(needle);
    for (; *hay; hay++) {
        if (to_lower_ascii(*hay) == to_lower_ascii(*needle)) {
            bool match = true;
            for (size_t i = 1; i < nlen; i++) {
                if (to_lower_ascii(hay[i]) != to_lower_ascii(needle[i])) {
                    match = false;
                    break;
                }
            }
            if (match) return hay;
        }
    }
    return NULL;
}

static void parse_url(const char *url, bool *out_https, char *host, size_t host_cap,
                      uint16_t *out_port, char *path, size_t path_cap)
{
    const char *p = url;
    while (*p == ' ') p++;

    bool is_https = true;
    uint16_t port = 443;

    if (str_istarts_with(p, "https://")) {
        is_https = true;
        port = 443;
        p += 8;
    } else if (str_istarts_with(p, "http://")) {
        is_https = false;
        port = 80;
        p += 7;
    } else {
        /* Default to http:// for private IPs / single-word Tailscale hostnames without dots */
        bool has_dot = false;
        for (const char *q = p; *q && *q != '/' && *q != ':'; q++) {
            if (*q == '.') has_dot = true;
        }
        bool is_ip = (*p >= '0' && *p <= '9');
        if (!has_dot || is_ip) {
            is_https = false;
            port = 80;
        }
    }

    size_t hi = 0;
    while (*p && *p != '/' && *p != ':' && *p != '?' && *p != '#') {
        if (hi + 1 < host_cap) host[hi++] = *p;
        p++;
    }
    host[hi] = 0;

    if (*p == ':') {
        p++;
        uint32_t pt = 0;
        while (*p >= '0' && *p <= '9') {
            pt = pt * 10u + (uint32_t)(*p - '0');
            p++;
        }
        if (pt > 0 && pt <= 65535u) port = (uint16_t)pt;
    }

    if (*p == '/' || *p == '?') {
        size_t pi = 0;
        if (*p == '?' && pi + 1 < path_cap) path[pi++] = '/';
        while (*p && *p != '#') {
            if (pi + 1 < path_cap) path[pi++] = *p;
            p++;
        }
        path[pi] = 0;
    } else {
        strncpy(path, "/", path_cap - 1);
        path[path_cap - 1] = 0;
    }

    *out_https = is_https;
    *out_port  = port;
}

static void url_decode_str(const char *src, size_t src_len, char *dst, size_t dst_cap)
{
    size_t r = 0, w = 0;
    while (r < src_len && w + 1 < dst_cap) {
        if (src[r] == '%' && r + 2 < src_len) {
            int hi = -1, lo = -1;
            char c1 = src[r + 1], c2 = src[r + 2];
            if (c1 >= '0' && c1 <= '9') hi = c1 - '0';
            else if (c1 >= 'a' && c1 <= 'f') hi = c1 - 'a' + 10;
            else if (c1 >= 'A' && c1 <= 'F') hi = c1 - 'A' + 10;
            if (c2 >= '0' && c2 <= '9') lo = c2 - '0';
            else if (c2 >= 'a' && c2 <= 'f') lo = c2 - 'a' + 10;
            else if (c2 >= 'A' && c2 <= 'F') lo = c2 - 'A' + 10;
            if (hi >= 0 && lo >= 0) {
                dst[w++] = (char)((hi << 4) | lo);
                r += 3;
                continue;
            }
        } else if (src[r] == '+') {
            dst[w++] = ' ';
            r++;
            continue;
        }
        dst[w++] = src[r++];
    }
    dst[w] = 0;
}

static void resolve_relative_url(const char *base_url, const char *href, char *out_url, size_t out_cap)
{
    while (*href == ' ' || *href == '\t' || *href == '\r' || *href == '\n') href++;

    /* Unwrap DuckDuckGo Lite redirect links (e.g. //duckduckgo.com/l/?uddg=https%3A%2F%2F...) */
    const char *uddg = strstr(href, "uddg=");
    if (uddg) {
        uddg += 5;
        const char *end = strchr(uddg, '&');
        size_t ulen = end ? (size_t)(end - uddg) : strlen(uddg);
        url_decode_str(uddg, ulen, out_url, out_cap);
        if (str_istarts_with(out_url, "http://") || str_istarts_with(out_url, "https://")) {
            return;
        }
    }

    if (str_istarts_with(href, "http://") || str_istarts_with(href, "https://")) {
        strncpy(out_url, href, out_cap - 1);
        out_url[out_cap - 1] = 0;
        return;
    }

    bool https = true;
    char host[96] = "";
    uint16_t port = 443;
    char base_path[256] = "/";
    parse_url(base_url, &https, host, sizeof(host), &port, base_path, sizeof(base_path));
    const char *scheme = https ? "https" : "http";

    if (href[0] == '/' && href[1] == '/') {
        ksnprintf(out_url, out_cap, "%s:%s", scheme, href);
        return;
    }

    char port_part[16] = "";
    if ((https && port != 443) || (!https && port != 80)) {
        ksnprintf(port_part, sizeof(port_part), ":%u", (unsigned)port);
    }

    if (href[0] == '/') {
        ksnprintf(out_url, out_cap, "%s://%s%s%s", scheme, host, port_part, href);
        return;
    }

    /* Strip query and filename from base_path */
    char *q = strchr(base_path, '?');
    if (q) *q = 0;
    char *last_slash = strrchr(base_path, '/');
    if (last_slash) *(last_slash + 1) = 0;
    else strcpy(base_path, "/");

    ksnprintf(out_url, out_cap, "%s://%s%s%s%s", scheme, host, port_part, base_path, href);
}

static size_t decode_chunked_inplace(char *buf, size_t len)
{
    size_t r = 0, w = 0;
    while (r < len) {
        while (r < len && (buf[r] == '\r' || buf[r] == '\n' || buf[r] == ' ')) r++;
        if (r >= len) break;
        size_t csz = 0;
        bool any_hex = false;
        while (r < len) {
            char c = buf[r];
            int v = -1;
            if (c >= '0' && c <= '9') v = c - '0';
            else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
            if (v < 0) break;
            csz = (csz << 4) | (size_t)v;
            any_hex = true;
            r++;
        }
        if (!any_hex) {
            /* Fallback: remaining data is not chunked */
            while (r < len) buf[w++] = buf[r++];
            break;
        }
        while (r < len && buf[r] != '\n') r++;
        if (r < len && buf[r] == '\n') r++;
        if (csz == 0) break;
        if (r + csz > len) csz = len - r;
        memmove(buf + w, buf + r, csz);
        w += csz;
        r += csz;
        if (r < len && buf[r] == '\r') r++;
        if (r < len && buf[r] == '\n') r++;
    }
    buf[w] = 0;
    return w;
}

typedef void (*web_status_cb_t)(const char *line1, const char *line2);

static bool web_fetch_url(const char *initial_url, char *final_url, size_t final_url_cap,
                          char *html_buf, size_t html_cap, size_t *out_len,
                          int *out_status_code, char *err_msg, size_t err_cap,
                          web_status_cb_t status_cb)
{
    strncpy(final_url, initial_url, final_url_cap - 1);
    final_url[final_url_cap - 1] = 0;
    *out_len = 0;
    *out_status_code = 0;

    for (int hop = 0; hop < 6; hop++) {
        bool is_https = true;
        char host[96] = "";
        uint16_t port = 443;
        char path[320] = "/";
        parse_url(final_url, &is_https, host, sizeof(host), &port, path, sizeof(path));

        if (!host[0]) {
            ksnprintf(err_msg, err_cap, "Invalid URL: %s", final_url);
            return false;
        }

        if (status_cb) {
            char s1[64];
            ksnprintf(s1, sizeof(s1), "Connecting %s://%s:%u...",
                      is_https ? "https" : "http", host, (unsigned)port);
            status_cb(s1, path);
        }

        web_conn_t conn;
        if (!web_conn_open(&conn, host, port, is_https)) {
            ksnprintf(err_msg, err_cap, "Connection failed to %s:%u", host, (unsigned)port);
            return false;
        }

        char req[640];
        int req_len = ksnprintf(req, sizeof(req),
                                "GET %s HTTP/1.1\r\n"
                                "Host: %s\r\n"
                                "User-Agent: Mozilla/5.0 (MeOS/0.1; M5Stack-Tab5)\r\n"
                                "Accept: text/html,text/plain;q=0.9,*/*;q=0.8\r\n"
                                "Accept-Language: ja,en-US;q=0.9,en;q=0.8\r\n"
                                "Accept-Encoding: identity\r\n"
                                "Connection: close\r\n\r\n",
                                path, host);

        if (!web_conn_write(&conn, req, (size_t)req_len)) {
            web_conn_close(&conn);
            ksnprintf(err_msg, err_cap, "Failed to send HTTP request to %s", host);
            return false;
        }

        if (status_cb) {
            status_cb("Downloading HTTP response...", final_url);
        }

        size_t total = 0;
        int hdr_end_pos = -1;
        int content_length = -1;
        while (total + 1 < html_cap) {
            int r = web_conn_read(&conn, (uint8_t *)html_buf + total,
                                  (html_cap - 1 - total > 4096u) ? 4096u : (html_cap - 1 - total),
                                  3000);
            if (r <= 0) break;
            total += (size_t)r;
            html_buf[total] = 0;

            if (hdr_end_pos < 0) {
                const char *he = strstr(html_buf, "\r\n\r\n");
                if (he) {
                    hdr_end_pos = (int)(he - html_buf) + 4;
                    const char *cl = str_ifind(html_buf, "\r\nContent-Length:");
                    if (cl && cl < he) {
                        cl += 17;
                        while (*cl == ' ') cl++;
                        int v = 0;
                        while (*cl >= '0' && *cl <= '9') { v = v * 10 + (*cl - '0'); cl++; }
                        content_length = v;
                    }
                }
            }
            if (hdr_end_pos > 0 && content_length >= 0 &&
                total >= (size_t)hdr_end_pos + (size_t)content_length) {
                break;
            }
        }
        web_conn_close(&conn);

        if (total == 0) {
            ksnprintf(err_msg, err_cap, "Empty response from %s", host);
            return false;
        }

        /* Parse HTTP status code & headers */
        int status = 200;
        const char *sp = strchr(html_buf, ' ');
        if (sp && str_istarts_with(html_buf, "HTTP/")) {
            while (*sp == ' ') sp++;
            status = 0;
            while (*sp >= '0' && *sp <= '9') {
                status = status * 10 + (*sp - '0');
                sp++;
            }
        }
        *out_status_code = status;

        char *body = strstr(html_buf, "\r\n\r\n");
        size_t hdr_len = 0;
        if (body) {
            *body = 0;
            body += 4;
            hdr_len = (size_t)(body - html_buf);
        } else {
            body = html_buf;
        }

        /* Check for 3xx Redirect */
        if ((status == 301 || status == 302 || status == 303 || status == 307 || status == 308) &&
            body != html_buf) {
            const char *loc = str_ifind(html_buf, "\nLocation:");
            if (loc) {
                loc += 10;
                while (*loc == ' ' || *loc == '\t') loc++;
                char loc_url[320];
                size_t li = 0;
                while (*loc && *loc != '\r' && *loc != '\n' && li + 1 < sizeof(loc_url)) {
                    loc_url[li++] = *loc++;
                }
                loc_url[li] = 0;
                if (loc_url[0]) {
                    char next_url[320];
                    resolve_relative_url(final_url, loc_url, next_url, sizeof(next_url));
                    strncpy(final_url, next_url, final_url_cap - 1);
                    final_url[final_url_cap - 1] = 0;
                    continue;
                }
            }
        }

        bool is_chunked = (body != html_buf) &&
                          (str_ifind(html_buf, "Transfer-Encoding: chunked") != NULL);

        size_t body_len = total - hdr_len;
        if (body != html_buf) {
            memmove(html_buf, body, body_len);
            html_buf[body_len] = 0;
        }
        if (is_chunked) {
            body_len = decode_chunked_inplace(html_buf, body_len);
        }

        *out_len = body_len;
        return true;
    }

    ksnprintf(err_msg, err_cap, "Too many HTTP redirects");
    return false;
}

/* Extract a JSON string value after `"key":"` and unescape `\n`, `\"`, `\/`, `\uXXXX` */
static const char *json_extract_str(const char *json, const char *key_pat, char *out, size_t out_cap)
{
    if (out && out_cap > 0) out[0] = 0;
    const char *p = strstr(json, key_pat);
    if (!p) return NULL;
    p += strlen(key_pat);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return NULL;
    p++;
    size_t w = 0;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) {
            p++;
            char esc = *p++;
            char ch = esc;
            if (esc == 'n') ch = '\n';
            else if (esc == 'r') ch = '\r';
            else if (esc == 't') ch = '\t';
            else if (esc == '"' || esc == '\\' || esc == '/') ch = esc;
            else if (esc == 'u' && p[0] && p[1] && p[2] && p[3]) {
                uint32_t cp = 0;
                for (int k = 0; k < 4; k++) {
                    char c = p[k];
                    uint32_t v = (c >= '0' && c <= '9') ? (uint32_t)(c - '0') :
                                 (c >= 'a' && c <= 'f') ? (uint32_t)(c - 'a' + 10) :
                                 (c >= 'A' && c <= 'F') ? (uint32_t)(c - 'A' + 10) : 0;
                    cp = (cp << 4) | v;
                }
                p += 4;
                if (out && w + 4 < out_cap) {
                    if (cp < 0x80u) out[w++] = (char)cp;
                    else if (cp < 0x800u) {
                        out[w++] = (char)(0xC0u | (cp >> 6));
                        out[w++] = (char)(0x80u | (cp & 0x3Fu));
                    } else {
                        out[w++] = (char)(0xE0u | (cp >> 12));
                        out[w++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
                        out[w++] = (char)(0x80u | (cp & 0x3Fu));
                    }
                }
                continue;
            }
            if (out && w + 1 < out_cap) out[w++] = ch;
        } else {
            if (out && w + 1 < out_cap) out[w++] = *p;
            p++;
        }
    }
    if (out && out_cap > 0) {
        /* Trim incomplete trailing UTF-8 sequence if truncated */
        size_t back = 0;
        while (back < w && back < 3 && ((uint8_t)out[w - 1 - back] & 0xC0u) == 0x80u) {
            back++;
        }
        if (back < w) {
            uint8_t lead = (uint8_t)out[w - 1 - back];
            size_t need = ((lead & 0xE0u) == 0xC0u) ? 2 :
                          ((lead & 0xF0u) == 0xE0u) ? 3 :
                          ((lead & 0xF8u) == 0xF0u) ? 4 : 1;
            if (need > 1 && back + 1 < need) {
                w -= (back + 1);
            }
        }
        out[w] = 0;
    }
    return (*p == '"') ? (p + 1) : p;
}

/*
 * Fast-path for note.com article URLs (`https://note.com/<creator>/n/<note_key>`)
 * and creator pages (`https://note.com/<creator>` or `/all`).
 * Uses note.com's lightweight JSON APIs (20-32 KB instead of 250-750 KB Next.js bundles)
 * so main content and inline `<img>` figures always load cleanly and quickly.
 */
static bool note_try_fetch_clean_html(const char *url, char *final_url, size_t final_url_cap,
                                      char *html_buf, size_t html_cap, size_t *out_len,
                                      int *out_status_code, char *err_msg, size_t err_cap,
                                      web_status_cb_t status_cb)
{
    bool is_https = true;
    char host[96] = "";
    uint16_t port = 443;
    char path[320] = "/";
    parse_url(url, &is_https, host, sizeof(host), &port, path, sizeof(path));
    if (strcmp(host, "note.com") != 0) return false;

    /* Check for `/<creator>/n/<note_key>` */
    const char *n_sep = strstr(path, "/n/n");
    if (n_sep) {
        const char *note_key = n_sep + 3; /* starts with 'n' */
        char key_buf[48];
        size_t ki = 0;
        while (note_key[ki] && note_key[ki] != '/' && note_key[ki] != '?' && note_key[ki] != '#' && ki + 1 < sizeof(key_buf)) {
            key_buf[ki] = note_key[ki];
            ki++;
        }
        key_buf[ki] = 0;

        char creator[64] = "";
        size_t ci = 0;
        const char *cp = (path[0] == '/') ? (path + 1) : path;
        while (cp < n_sep && *cp != '/' && ci + 1 < sizeof(creator)) {
            creator[ci++] = *cp++;
        }
        creator[ci] = 0;

        char api_url[160];
        ksnprintf(api_url, sizeof(api_url), "https://note.com/api/v3/notes/%s", key_buf);
        char api_final[160];
        size_t json_len = 0;
        if (!web_fetch_url(api_url, api_final, sizeof(api_final),
                           html_buf, html_cap, &json_len, out_status_code, err_msg, err_cap, status_cb)) {
            return false;
        }

        char *title = (char *)kmalloc_psram(512);
        char *eyecatch = (char *)kmalloc_psram(512);
        char *pub_at = (char *)kmalloc_psram(128);
        char *body = (char *)kmalloc_psram(128u * 1024u);
        if (!title || !eyecatch || !pub_at || !body) {
            if (title) kfree(title);
            if (eyecatch) kfree(eyecatch);
            if (pub_at) kfree(pub_at);
            if (body) kfree(body);
            return false;
        }

        json_extract_str(html_buf, "\"name\":", title, 512);
        json_extract_str(html_buf, "\"eyecatch\":", eyecatch, 512);
        json_extract_str(html_buf, "\"publish_at\":", pub_at, 128);
        json_extract_str(html_buf, "\"body\":", body, 128u * 1024u);

        if (body[0]) {
            strncpy(final_url, url, final_url_cap - 1);
            final_url[final_url_cap - 1] = 0;
            int n = ksnprintf(html_buf, html_cap,
                              "<html><head><title>%s | note</title></head><body>"
                              "<h1>%s</h1>"
                              "<p>Author: <a href=\"https://note.com/%s\">%s</a>  (%s)</p>"
                              "%s%s%s"
                              "<hr/>%s"
                              "<hr/><p><a href=\"https://note.com/%s\">Back to %s note index</a></p>"
                              "</body></html>",
                              title[0] ? title : key_buf,
                              title[0] ? title : key_buf,
                              creator, creator, pub_at,
                              eyecatch[0] ? "<p><img src=\"" : "",
                              eyecatch[0] ? eyecatch : "",
                              eyecatch[0] ? "\"/></p>" : "",
                              body,
                              creator, creator);
            *out_len = (n > 0) ? (size_t)n : strlen(html_buf);
            kfree(body);
            kfree(pub_at);
            kfree(eyecatch);
            kfree(title);
            return true;
        }
        kfree(body);
        kfree(pub_at);
        kfree(eyecatch);
        kfree(title);
        return false;
    }

    /* Check for creator index `/<creator>` or `/<creator>/all` */
    if (path[0] == '/' && path[1] != 0 && strncmp(path, "/api/", 5) != 0) {
        char creator[64] = "";
        size_t ci = 0;
        const char *cp = path + 1;
        while (*cp && *cp != '/' && *cp != '?' && *cp != '#' && ci + 1 < sizeof(creator)) {
            creator[ci++] = *cp++;
        }
        creator[ci] = 0;
        if (creator[0] && (*cp == 0 || strcmp(cp, "/") == 0 || strncmp(cp, "/all", 4) == 0)) {
            char api_url[192];
            ksnprintf(api_url, sizeof(api_url),
                      "https://note.com/api/v2/creators/%s/contents?kind=note&page=1", creator);
            char api_final[192];
            size_t json_len = 0;
            if (web_fetch_url(api_url, api_final, sizeof(api_final),
                              html_buf, html_cap, &json_len, out_status_code, err_msg, err_cap, status_cb)) {
                char *out_html = (char *)kmalloc_psram(128u * 1024u);
                if (out_html) {
                    size_t w = (size_t)ksnprintf(out_html, 128u * 1024u,
                                                 "<html><head><title>%s | note</title></head><body>"
                                                 "<h1>%s の記事一覧 (note)</h1><hr/>",
                                                 creator, creator);
                    const char *cur = html_buf;
                    int count = 0;
                    while (count < 12 && w + 2048 < 128u * 1024u) {
                        char n_title[256] = "";
                        char n_pub[64] = "";
                        char n_body[640] = "";
                        char n_url[256] = "";
                        const char *after_name = json_extract_str(cur, "\"name\":", n_title, sizeof(n_title));
                        if (!after_name) break;
                        const char *after_pub = json_extract_str(after_name, "\"publishAt\":", n_pub, sizeof(n_pub));
                        if (!after_pub) break;
                        json_extract_str(after_pub, "\"body\":", n_body, sizeof(n_body));
                        const char *after_url = json_extract_str(after_pub, "\"noteUrl\":", n_url, sizeof(n_url));
                        if (!after_url || !n_url[0]) break;
                        w += (size_t)ksnprintf(out_html + w, 128u * 1024u - w,
                                               "<h2><a href=\"%s\">%s</a></h2>"
                                               "<p>[%s]</p><p>%s</p><hr/>",
                                               n_url, n_title, n_pub, n_body);
                        cur = after_url;
                        count++;
                    }
                    if (count > 0) {
                        w += (size_t)ksnprintf(out_html + w, 128u * 1024u - w, "</body></html>");
                        memcpy(html_buf, out_html, w + 1);
                        *out_len = w;
                        strncpy(final_url, url, final_url_cap - 1);
                        final_url[final_url_cap - 1] = 0;
                        kfree(out_html);
                        return true;
                    }
                    kfree(out_html);
                }
            }
        }
    }
    return false;
}

/* =========================================================================
 * 3b. Pure-C ZLIB/DEFLATE (RFC 1950/1951) + PNG Image Decoder & Fetcher
 * ========================================================================= */

typedef struct {
    const uint8_t *src;
    size_t         src_len;
    size_t         byte_pos;
    uint32_t       bit_buf;
    int            bit_cnt;
} deflate_bs_t;

static uint32_t bs_read_bits(deflate_bs_t *bs, int n)
{
    while (bs->bit_cnt < n) {
        uint8_t b = (bs->byte_pos < bs->src_len) ? bs->src[bs->byte_pos++] : 0;
        bs->bit_buf |= ((uint32_t)b << bs->bit_cnt);
        bs->bit_cnt += 8;
    }
    uint32_t val = bs->bit_buf & ((1u << n) - 1u);
    bs->bit_buf >>= n;
    bs->bit_cnt -= n;
    return val;
}

typedef struct {
    uint16_t counts[16];
    uint16_t symbols[288];
} huff_tbl_t;

static void huff_build(huff_tbl_t *t, const uint8_t *lengths, int num_syms)
{
    uint16_t offs[16];
    memset(t->counts, 0, sizeof(t->counts));
    for (int i = 0; i < num_syms; i++) {
        if (lengths[i] > 0 && lengths[i] <= 15) t->counts[lengths[i]]++;
    }
    uint16_t sum = 0;
    for (int bits = 1; bits <= 15; bits++) {
        offs[bits] = sum;
        sum += t->counts[bits];
    }
    for (int i = 0; i < num_syms; i++) {
        uint8_t len = lengths[i];
        if (len > 0 && len <= 15) {
            t->symbols[offs[len]++] = (uint16_t)i;
        }
    }
}

static int huff_decode(deflate_bs_t *bs, const huff_tbl_t *t)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        code |= (int)bs_read_bits(bs, 1);
        int count = t->counts[len];
        if (code - count < first) {
            return t->symbols[index + (code - first)];
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static bool zlib_inflate(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_cap, size_t *out_len)
{
    if (src_len < 6) return false;
    size_t start = 2;
    if (src[1] & 0x20u) start += 4;
    if (start >= src_len) return false;

    deflate_bs_t bs = { src, src_len, start, 0, 0 };
    size_t out_pos = 0;

    static const uint16_t len_base[29] = {
        3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,
        35,43,51,59,67,83,99,115,131,163,195,227,258
    };
    static const uint8_t len_extra[29] = {
        0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,
        3,3,3,3,4,4,4,4,5,5,5,5,0
    };
    static const uint16_t dist_base[30] = {
        1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,
        257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577
    };
    static const uint8_t dist_extra[30] = {
        0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,
        7,7,8,8,9,9,10,10,11,11,12,12,13,13
    };
    static const uint8_t clen_order[19] = {
        16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
    };

    huff_tbl_t *lit_tbl  = (huff_tbl_t *)kmalloc_psram(sizeof(huff_tbl_t));
    huff_tbl_t *dist_tbl = (huff_tbl_t *)kmalloc_psram(sizeof(huff_tbl_t));
    huff_tbl_t *cl_tbl   = (huff_tbl_t *)kmalloc_psram(sizeof(huff_tbl_t));
    if (!lit_tbl || !dist_tbl || !cl_tbl) {
        if (lit_tbl)  kfree(lit_tbl);
        if (dist_tbl) kfree(dist_tbl);
        if (cl_tbl)   kfree(cl_tbl);
        return false;
    }

    uint8_t lengths[320];
    bool bfinal = false;
    bool ok = true;

    while (!bfinal && bs.byte_pos <= bs.src_len) {
        bfinal = bs_read_bits(&bs, 1) != 0;
        uint32_t btype = bs_read_bits(&bs, 2);
        if (btype == 0) {
            bs.bit_buf = 0;
            bs.bit_cnt = 0;
            if (bs.byte_pos + 4 > bs.src_len) { ok = false; break; }
            uint16_t blen = (uint16_t)bs.src[bs.byte_pos] | ((uint16_t)bs.src[bs.byte_pos + 1] << 8);
            bs.byte_pos += 4;
            if (bs.byte_pos + blen > bs.src_len || out_pos + blen > dst_cap) { ok = false; break; }
            memcpy(dst + out_pos, bs.src + bs.byte_pos, blen);
            bs.byte_pos += blen;
            out_pos += blen;
        } else if (btype == 1 || btype == 2) {
            if (btype == 1) {
                for (int i = 0; i < 144; i++)   lengths[i] = 8;
                for (int i = 144; i < 256; i++) lengths[i] = 9;
                for (int i = 256; i < 280; i++) lengths[i] = 7;
                for (int i = 280; i < 288; i++) lengths[i] = 8;
                huff_build(lit_tbl, lengths, 288);
                for (int i = 0; i < 32; i++)    lengths[i] = 5;
                huff_build(dist_tbl, lengths, 32);
            } else {
                int hlit  = (int)bs_read_bits(&bs, 5) + 257;
                int hdist = (int)bs_read_bits(&bs, 5) + 1;
                int hclen = (int)bs_read_bits(&bs, 4) + 4;
                uint8_t clen[19];
                memset(clen, 0, sizeof(clen));
                for (int i = 0; i < hclen; i++) {
                    clen[clen_order[i]] = (uint8_t)bs_read_bits(&bs, 3);
                }
                huff_build(cl_tbl, clen, 19);
                int idx = 0;
                int total_codes = hlit + hdist;
                memset(lengths, 0, sizeof(lengths));
                while (idx < total_codes) {
                    int sym = huff_decode(&bs, cl_tbl);
                    if (sym < 0) { ok = false; break; }
                    if (sym < 16) {
                        lengths[idx++] = (uint8_t)sym;
                    } else if (sym == 16) {
                        int rep = (int)bs_read_bits(&bs, 2) + 3;
                        uint8_t prev = (idx > 0) ? lengths[idx - 1] : 0;
                        while (rep-- > 0 && idx < total_codes) lengths[idx++] = prev;
                    } else if (sym == 17) {
                        int rep = (int)bs_read_bits(&bs, 3) + 3;
                        while (rep-- > 0 && idx < total_codes) lengths[idx++] = 0;
                    } else if (sym == 18) {
                        int rep = (int)bs_read_bits(&bs, 7) + 11;
                        while (rep-- > 0 && idx < total_codes) lengths[idx++] = 0;
                    }
                }
                if (!ok) break;
                huff_build(lit_tbl, lengths, hlit);
                huff_build(dist_tbl, lengths + hlit, hdist);
            }

            for (;;) {
                int sym = huff_decode(&bs, lit_tbl);
                if (sym < 0) { ok = false; break; }
                if (sym < 256) {
                    if (out_pos >= dst_cap) { ok = false; break; }
                    dst[out_pos++] = (uint8_t)sym;
                } else if (sym == 256) {
                    break;
                } else {
                    int lidx = sym - 257;
                    if (lidx < 0 || lidx >= 29) { ok = false; break; }
                    int length = (int)len_base[lidx] + (int)bs_read_bits(&bs, len_extra[lidx]);
                    int dsym = huff_decode(&bs, dist_tbl);
                    if (dsym < 0 || dsym >= 30) { ok = false; break; }
                    size_t dist = (size_t)dist_base[dsym] + (size_t)bs_read_bits(&bs, dist_extra[dsym]);
                    if (dist == 0 || dist > out_pos || out_pos + (size_t)length > dst_cap) { ok = false; break; }
                    for (int k = 0; k < length; k++) {
                        dst[out_pos] = dst[out_pos - dist];
                        out_pos++;
                    }
                }
            }
            if (!ok) break;
        } else {
            ok = false;
            break;
        }
    }

    kfree(cl_tbl);
    kfree(dist_tbl);
    kfree(lit_tbl);
    *out_len = out_pos;
    return ok;
}

static uint8_t paeth_pred(int a, int b, int c)
{
    int p = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return (uint8_t)a;
    if (pb <= pc) return (uint8_t)b;
    return (uint8_t)c;
}

static bool decode_png_rgb565(const uint8_t *png, size_t png_len, int max_w, int max_h,
                              uint16_t **out_px, int *out_w, int *out_h)
{
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    if (png_len < 33 || memcmp(png, sig, 8) != 0) return false;

    uint32_t w = 0, h = 0;
    uint8_t bd = 0, ct = 0, ilace = 0;
    uint8_t (*pal)[3] = (uint8_t (*)[3])kmalloc_psram(256 * 3);
    uint8_t *idat = (uint8_t *)kmalloc_psram(png_len);
    if (!pal || !idat) {
        if (pal)  kfree(pal);
        if (idat) kfree(idat);
        return false;
    }
    memset(pal, 0, 256 * 3);
    size_t idat_len = 0;

    size_t pos = 8;
    while (pos + 8 <= png_len) {
        uint32_t clen = get_be32(png + pos);
        const uint8_t *ctype = png + pos + 4;
        const uint8_t *cdata = png + pos + 8;
        if (pos + 12 + clen > png_len) break;
        if (memcmp(ctype, "IHDR", 4) == 0 && clen >= 13) {
            w     = get_be32(cdata);
            h     = get_be32(cdata + 4);
            bd    = cdata[8];
            ct    = cdata[9];
            ilace = cdata[12];
        } else if (memcmp(ctype, "PLTE", 4) == 0) {
            int n = (int)(clen / 3u);
            if (n > 256) n = 256;
            for (int i = 0; i < n; i++) {
                pal[i][0] = cdata[i * 3 + 0];
                pal[i][1] = cdata[i * 3 + 1];
                pal[i][2] = cdata[i * 3 + 2];
            }
        } else if (memcmp(ctype, "IDAT", 4) == 0) {
            if (idat_len + clen <= png_len) {
                memcpy(idat + idat_len, cdata, clen);
                idat_len += clen;
            }
        } else if (memcmp(ctype, "IEND", 4) == 0) {
            break;
        }
        pos += 12 + clen;
    }

    if (w == 0 || h == 0 || w > 1024 || h > 1024 || bd != 8 || ilace != 0 || idat_len == 0) {
        kfree(idat);
        kfree(pal);
        return false;
    }

    int bpp = (ct == 6) ? 4 : (ct == 2) ? 3 : (ct == 4) ? 2 : 1;
    size_t stride  = (size_t)w * (size_t)bpp;
    size_t raw_cap = (size_t)h * (1u + stride);
    uint8_t *raw   = (uint8_t *)kmalloc_psram(raw_cap);
    if (!raw) {
        kfree(idat);
        kfree(pal);
        return false;
    }

    size_t raw_len = 0;
    if (!zlib_inflate(idat, idat_len, raw, raw_cap, &raw_len) || raw_len < raw_cap) {
        kfree(raw);
        kfree(idat);
        kfree(pal);
        return false;
    }
    kfree(idat);

    for (uint32_t y = 0; y < h; y++) {
        uint8_t *row  = raw + (size_t)y * (1u + stride);
        uint8_t ftype = row[0];
        uint8_t *cur  = row + 1;
        const uint8_t *prev = (y > 0) ? (row - (1u + stride) + 1) : NULL;
        for (size_t i = 0; i < stride; i++) {
            uint8_t a = (i >= (size_t)bpp) ? cur[i - bpp] : 0;
            uint8_t b = prev ? prev[i] : 0;
            uint8_t c = (prev && i >= (size_t)bpp) ? prev[i - bpp] : 0;
            if (ftype == 1)      cur[i] = (uint8_t)(cur[i] + a);
            else if (ftype == 2) cur[i] = (uint8_t)(cur[i] + b);
            else if (ftype == 3) cur[i] = (uint8_t)(cur[i] + ((a + b) >> 1));
            else if (ftype == 4) cur[i] = (uint8_t)(cur[i] + paeth_pred(a, b, c));
        }
    }

    int dw = (int)w;
    int dh = (int)h;
    if (dw > max_w) {
        dh = (dh * max_w) / dw;
        dw = max_w;
    }
    if (dh > max_h) {
        dw = (dw * max_h) / dh;
        dh = max_h;
    }
    dh = ((dh + 15) / 16) * 16;
    if (dh < 16) dh = 16;
    if (dw < 16) dw = 16;

    uint16_t *px = (uint16_t *)kmalloc_psram((size_t)dw * (size_t)dh * sizeof(uint16_t));
    if (!px) {
        kfree(raw);
        kfree(pal);
        return false;
    }

    for (int y = 0; y < dh; y++) {
        uint32_t sy = ((uint32_t)y * h) / (uint32_t)dh;
        if (sy >= h) sy = h - 1;
        const uint8_t *srow = raw + (size_t)sy * (1u + stride) + 1;
        for (int x = 0; x < dw; x++) {
            uint32_t sx = ((uint32_t)x * w) / (uint32_t)dw;
            if (sx >= w) sx = w - 1;
            uint8_t r = 0, g = 0, b = 0;
            if (ct == 3) {
                uint8_t idx = srow[sx];
                r = pal[idx][0]; g = pal[idx][1]; b = pal[idx][2];
            } else if (ct == 2 || ct == 6) {
                r = srow[sx * bpp + 0];
                g = srow[sx * bpp + 1];
                b = srow[sx * bpp + 2];
            } else {
                r = g = b = srow[sx * bpp];
            }
            px[y * dw + x] = RGB565(r, g, b);
        }
    }

    kfree(raw);
    kfree(pal);
    *out_px = px;
    *out_w  = dw;
    *out_h  = dh;
    return true;
}

/* Fetch binary data (e.g. PNG image) over HTTP/HTTPS */
static bool web_fetch_binary(const char *img_url, uint8_t *buf, size_t buf_cap, size_t *out_len)
{
    *out_len = 0;
    char cur_url[320];
    strncpy(cur_url, img_url, sizeof(cur_url) - 1);
    cur_url[sizeof(cur_url) - 1] = 0;

    /* For assets.st-note.com images, request compact 280px PNG */
    if (strstr(cur_url, "assets.st-note.com/")) {
        char *q = strchr(cur_url, '?');
        if (q) *q = 0;
        size_t l = strlen(cur_url);
        ksnprintf(cur_url + l, sizeof(cur_url) - l, "?width=280&format=png");
    }

    bool is_https = true;
    char host[96] = "";
    uint16_t port = 443;
    char path[320] = "/";
    parse_url(cur_url, &is_https, host, sizeof(host), &port, path, sizeof(path));
    if (!host[0]) return false;

    for (int attempt = 0; attempt < 2; attempt++) {
        if (attempt > 0) task_sleep(80);

        web_conn_t conn;
        if (!web_conn_open(&conn, host, port, is_https)) continue;

        char req[512];
        int req_len = ksnprintf(req, sizeof(req),
                                "GET %s HTTP/1.1\r\n"
                                "Host: %s\r\n"
                                "User-Agent: Mozilla/5.0 (MeOS/0.1; M5Stack-Tab5)\r\n"
                                "Accept: image/png,*/*;q=0.8\r\n"
                                "Accept-Encoding: identity\r\n"
                                "Connection: close\r\n\r\n",
                                path, host);
        if (!web_conn_write(&conn, req, (size_t)req_len)) {
            web_conn_close(&conn);
            continue;
        }

        size_t total = 0;
        int hdr_end_pos = -1;
        int content_length = -1;
        while (total + 1 < buf_cap) {
            int r = web_conn_read(&conn, buf + total,
                                  (buf_cap - 1 - total > 4096u) ? 4096u : (buf_cap - 1 - total),
                                  3000);
            if (r <= 0) break;
            total += (size_t)r;
            buf[total] = 0;

            if (hdr_end_pos < 0) {
                const char *he = strstr((const char *)buf, "\r\n\r\n");
                if (he) {
                    hdr_end_pos = (int)(he - (const char *)buf) + 4;
                    const char *cl = str_ifind((const char *)buf, "\r\nContent-Length:");
                    if (cl && cl < he) {
                        cl += 17;
                        while (*cl == ' ') cl++;
                        int v = 0;
                        while (*cl >= '0' && *cl <= '9') { v = v * 10 + (*cl - '0'); cl++; }
                        content_length = v;
                    }
                }
            }
            if (hdr_end_pos > 0 && content_length >= 0 &&
                total >= (size_t)hdr_end_pos + (size_t)content_length) {
                break;
            }
        }
        web_conn_close(&conn);

        if (hdr_end_pos <= 0 || total <= (size_t)hdr_end_pos) continue;
        if (content_length > 0 && total < (size_t)hdr_end_pos + (size_t)content_length) continue;

        bool is_chunked = (str_ifind((const char *)buf, "Transfer-Encoding: chunked") != NULL &&
                           content_length < 0);
        size_t body_len = total - (size_t)hdr_end_pos;
        memmove(buf, buf + hdr_end_pos, body_len);
        if (is_chunked) {
            body_len = decode_chunked_inplace((char *)buf, body_len);
        }
        *out_len = body_len;
        if (body_len > 32) return true;
    }
    return false;
}

/* =========================================================================
 * 4. HTML + UTF-8 Japanese Parser & Layout Engine
 * ========================================================================= */

typedef struct {
    uint16_t cp[WEB_COLS];
    uint8_t  style[WEB_COLS];   /* 0=normal, 1=heading1, 2=heading2, 3=link, 4=code/pre, 5=dim/quote */
    int16_t  link_id[WEB_COLS]; /* -1 if not a link, or 0..WEB_MAX_LINKS-1 */
    int16_t  img_id;            /* -1 if text row, or 0..WEB_MAX_IMAGES-1 */
    uint16_t img_row_y;         /* pixel row offset within decoded image */
} web_line_t;

typedef struct {
    char url[256];
    int  line_idx;
} web_link_t;

typedef struct {
    char      url[320];
    uint16_t *pixels;
    int       w;
    int       h;
} web_image_t;

typedef struct {
    char        current_url[320];
    char        title[96];
    int         status_code;
    size_t      raw_bytes;
    char       *raw_html;
    web_line_t *lines;
    int         line_count;
    web_link_t *links;
    int         link_count;
    web_image_t images[WEB_MAX_IMAGES];
    int         image_count;
    int         scroll_y;
    int         sel_link;
    bool        source_mode;
} web_doc_t;

typedef struct {
    web_doc_t *doc;
    int        cx;
    uint8_t    cur_style;
    int16_t    cur_link;
    bool       in_pre;
    bool       last_was_space;
    bool       last_was_blank_line;
} html_layout_t;

static void web_free_images(web_doc_t *doc)
{
    for (int i = 0; i < doc->image_count; i++) {
        if (doc->images[i].pixels) {
            kfree(doc->images[i].pixels);
            doc->images[i].pixels = NULL;
        }
    }
    doc->image_count = 0;
}

static void layout_init_line(web_line_t *ln)
{
    for (int i = 0; i < WEB_COLS; i++) {
        ln->cp[i]      = ' ';
        ln->style[i]   = 0;
        ln->link_id[i] = -1;
    }
    ln->img_id    = -1;
    ln->img_row_y = 0;
}

static void layout_newline(html_layout_t *ly, bool allow_blank)
{
    if (ly->cx == 0) {
        if (!allow_blank || ly->last_was_blank_line) return;
        ly->last_was_blank_line = true;
    } else {
        ly->last_was_blank_line = false;
    }
    if (ly->doc->line_count + 1 < WEB_MAX_LINES) {
        ly->doc->line_count++;
        layout_init_line(&ly->doc->lines[ly->doc->line_count]);
    }
    ly->cx = 0;
    ly->last_was_space = true;
}

static void layout_put_cp(html_layout_t *ly, uint32_t cp)
{
    if (cp == '\r') return;
    if (cp == '\n') {
        if (ly->in_pre) {
            layout_newline(ly, true);
        } else if (!ly->last_was_space) {
            layout_put_cp(ly, ' ');
        }
        return;
    }
    if (cp == '\t') cp = ' ';
    if (cp < 0x20u) return;

    if (!ly->in_pre && cp == ' ') {
        if (ly->last_was_space || ly->cx == 0) return;
        ly->last_was_space = true;
    } else {
        ly->last_was_space = false;
    }

    if (ly->cx >= WEB_COLS - 1) {
        int prev_cx = ly->cx;
        ly->cx = 0;
        ly->last_was_blank_line = false;
        if (ly->doc->line_count + 1 < WEB_MAX_LINES) {
            ly->doc->line_count++;
            layout_init_line(&ly->doc->lines[ly->doc->line_count]);
        }
        (void)prev_cx;
        if (!ly->in_pre && cp == ' ') return;
    }

    web_line_t *ln = &ly->doc->lines[ly->doc->line_count];
    ln->cp[ly->cx]      = (uint16_t)(cp <= 0xFFFFu ? cp : 0xFFFDu);
    ln->style[ly->cx]   = ly->cur_style;
    ln->link_id[ly->cx] = ly->cur_link;
    ly->cx++;
    ly->last_was_blank_line = false;
}

static void layout_put_utf8(html_layout_t *ly, const char *s)
{
    const uint8_t *p = (const uint8_t *)s;
    while (*p) {
        uint8_t b0 = *p++;
        uint32_t cp = b0;
        if ((b0 & 0xE0u) == 0xC0u && (p[0] & 0xC0u) == 0x80u) {
            cp = ((uint32_t)(b0 & 0x1Fu) << 6) | (uint32_t)(p[0] & 0x3Fu);
            p += 1;
        } else if ((b0 & 0xF0u) == 0xE0u && (p[0] & 0xC0u) == 0x80u && (p[1] & 0xC0u) == 0x80u) {
            cp = ((uint32_t)(b0 & 0x0Fu) << 12) | ((uint32_t)(p[0] & 0x3Fu) << 6) | (uint32_t)(p[1] & 0x3Fu);
            p += 2;
        } else if ((b0 & 0xF8u) == 0xF0u && (p[0] & 0xC0u) == 0x80u && (p[1] & 0xC0u) == 0x80u && (p[2] & 0xC0u) == 0x80u) {
            cp = 0xFFFDu;
            p += 3;
        }
        layout_put_cp(ly, cp);
    }
}

/* Decode an HTML entity starting at `&` and advance `*p_ptr` */
static uint32_t decode_html_entity(const char **p_ptr)
{
    const char *p = *p_ptr + 1; /* skip '&' */
    if (*p == '#') {
        p++;
        uint32_t val = 0;
        if (*p == 'x' || *p == 'X') {
            p++;
            while ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')) {
                uint32_t d = (*p >= 'a') ? (uint32_t)(*p - 'a' + 10) :
                             ((*p >= 'A') ? (uint32_t)(*p - 'A' + 10) : (uint32_t)(*p - '0'));
                val = (val << 4) | d;
                p++;
            }
        } else {
            while (*p >= '0' && *p <= '9') {
                val = val * 10u + (uint32_t)(*p - '0');
                p++;
            }
        }
        if (*p == ';') p++;
        *p_ptr = p;
        return (val > 0 && val <= 0xFFFFu) ? val : ' ';
    }

    struct { const char *name; uint16_t cp; } static const ents[] = {
        { "amp;",    '&'    },
        { "lt;",     '<'    },
        { "gt;",     '>'    },
        { "quot;",   '"'    },
        { "apos;",   '\''   },
        { "nbsp;",   ' '    },
        { "copy;",   0x00A9 },
        { "reg;",    0x00AE },
        { "yen;",    0x00A5 },
        { "mdash;",  0x2014 },
        { "ndash;",  0x2013 },
        { "hellip;", 0x2026 },
        { "bull;",   0x2022 },
        { "rarr;",   0x2192 },
        { "larr;",   0x2190 },
        { "laquo;",  0x00AB },
        { "raquo;",  0x00BB },
    };
    for (size_t i = 0; i < sizeof(ents) / sizeof(ents[0]); i++) {
        size_t elen = strlen(ents[i].name);
        if (str_istarts_with(p, ents[i].name)) {
            *p_ptr = p + elen;
            return ents[i].cp;
        }
    }

    *p_ptr = p;
    return '&';
}

static bool extract_attr_val(const char *tag_body, const char *attr_name, char *out_val, size_t out_cap)
{
    size_t alen = strlen(attr_name);
    const char *h = str_ifind(tag_body, attr_name);
    while (h) {
        if (h > tag_body) {
            char prev = h[-1];
            if ((prev >= 'a' && prev <= 'z') || (prev >= 'A' && prev <= 'Z') ||
                (prev >= '0' && prev <= '9') || prev == '-' || prev == '_') {
                h = str_ifind(h + alen, attr_name);
                continue;
            }
        }
        const char *after = h + alen;
        while (*after == ' ' || *after == '\t' || *after == '\r' || *after == '\n') after++;
        if (*after == '=') {
            after++;
            while (*after == ' ' || *after == '\t' || *after == '\r' || *after == '\n') after++;
            char quote = 0;
            if (*after == '"' || *after == '\'') quote = *after++;
            size_t i = 0;
            while (*after && i + 1 < out_cap) {
                if (quote && *after == quote) break;
                if (!quote && (*after == ' ' || *after == '\t' || *after == '>' || *after == '\r' || *after == '\n')) break;
                if (*after == '&') {
                    uint32_t cp = decode_html_entity(&after);
                    out_val[i++] = (char)(cp < 0x80u ? cp : '?');
                    continue;
                }
                out_val[i++] = *after++;
            }
            out_val[i] = 0;
            return i > 0;
        }
        h = str_ifind(after, attr_name);
    }
    return false;
}

static bool extract_attr_href(const char *tag_body, char *out_href, size_t out_cap)
{
    return extract_attr_val(tag_body, "href", out_href, out_cap);
}

static void gui_status_popup(const char *line1, const char *line2);

static void web_render_document(web_doc_t *doc, void (*progress_cb)(const char *msg, const char *sub))
{
    web_free_images(doc);
    doc->line_count = 0;
    doc->link_count = 0;
    layout_init_line(&doc->lines[0]);

    if (!doc->raw_html || doc->raw_bytes == 0) {
        doc->line_count = 1;
        return;
    }

    html_layout_t ly;
    memset(&ly, 0, sizeof(ly));
    ly.doc      = doc;
    ly.cur_link = -1;

    if (doc->source_mode) {
        ly.in_pre    = true;
        ly.cur_style = 4;
        layout_put_utf8(&ly, doc->raw_html);
        doc->line_count++;
        return;
    }

    /* Extract <title> if present */
    strcpy(doc->title, doc->current_url);
    const char *tstart = str_ifind(doc->raw_html, "<title");
    if (tstart) {
        const char *gt = strchr(tstart, '>');
        if (gt) {
            gt++;
            const char *tend = str_ifind(gt, "</title>");
            if (tend && tend > gt) {
                size_t ti = 0;
                const char *tp = gt;
                while (tp < tend && ti + 1 < sizeof(doc->title)) {
                    if (*tp == '\r' || *tp == '\n' || *tp == '\t') {
                        if (ti > 0 && doc->title[ti - 1] != ' ') doc->title[ti++] = ' ';
                        tp++;
                    } else {
                        doc->title[ti++] = *tp++;
                    }
                }
                doc->title[ti] = 0;
            }
        }
    }

    const char *p = doc->raw_html;
    while (*p && doc->line_count < WEB_MAX_LINES - 2) {
        if (*p == '<') {
            /* Check for HTML comment <!-- ... --> */
            if (strncmp(p, "<!--", 4) == 0) {
                const char *cend = strstr(p + 4, "-->");
                if (cend) { p = cend + 3; continue; }
            }

            const char *gt = strchr(p + 1, '>');
            if (!gt) break;

            char tag_buf[1024];
            size_t tlen = (size_t)(gt - (p + 1));
            if (tlen >= sizeof(tag_buf)) tlen = sizeof(tag_buf) - 1;
            memcpy(tag_buf, p + 1, tlen);
            tag_buf[tlen] = 0;

            p = gt + 1;

            const char *tn = tag_buf;
            while (*tn == ' ' || *tn == '\t') tn++;
            bool is_close = (*tn == '/');
            if (is_close) tn++;
            while (*tn == ' ') tn++;

            char name[24];
            size_t ni = 0;
            while (*tn && *tn != ' ' && *tn != '\t' && *tn != '/' && *tn != '\r' && *tn != '\n' && ni + 1 < sizeof(name)) {
                name[ni++] = to_lower_ascii(*tn++);
            }
            name[ni] = 0;

            /* Skip invisible blocks: <head>, <script>, <style>, <svg>, <noscript>, <select> */
            if (!is_close && (strcmp(name, "script") == 0 || strcmp(name, "style") == 0 ||
                              strcmp(name, "svg") == 0    || strcmp(name, "head") == 0  ||
                              strcmp(name, "noscript") == 0 || strcmp(name, "select") == 0)) {
                char close_pat[32];
                ksnprintf(close_pat, sizeof(close_pat), "</%s>", name);
                const char *end_blk = str_ifind(p, close_pat);
                if (end_blk) {
                    p = end_blk + strlen(close_pat);
                }
                continue;
            }

            if (strcmp(name, "br") == 0) {
                layout_newline(&ly, true);
            } else if (strcmp(name, "p") == 0 || strcmp(name, "div") == 0 ||
                       strcmp(name, "section") == 0 || strcmp(name, "article") == 0 ||
                       strcmp(name, "header") == 0  || strcmp(name, "footer") == 0  ||
                       strcmp(name, "figure") == 0  || strcmp(name, "figcaption") == 0 ||
                       strcmp(name, "tr") == 0      || strcmp(name, "dt") == 0      ||
                       strcmp(name, "dd") == 0      || strcmp(name, "form") == 0) {
                layout_newline(&ly, strcmp(name, "p") == 0);
            } else if (strcmp(name, "h1") == 0 || strcmp(name, "h2") == 0 || strcmp(name, "h3") == 0) {
                layout_newline(&ly, true);
                if (!is_close) {
                    ly.cur_style = 1;
                    layout_put_utf8(&ly, "■ ");
                } else {
                    ly.cur_style = 0;
                    layout_newline(&ly, true);
                }
            } else if (strcmp(name, "h4") == 0 || strcmp(name, "h5") == 0 || strcmp(name, "h6") == 0) {
                layout_newline(&ly, false);
                ly.cur_style = is_close ? 0 : 2;
            } else if (strcmp(name, "li") == 0) {
                if (!is_close) {
                    layout_newline(&ly, false);
                    layout_put_utf8(&ly, "  ・");
                }
            } else if (strcmp(name, "hr") == 0) {
                layout_newline(&ly, false);
                uint8_t prev = ly.cur_style;
                ly.cur_style = 5;
                for (int i = 0; i < 76; i++) layout_put_cp(&ly, '-');
                ly.cur_style = prev;
                layout_newline(&ly, false);
            } else if (strcmp(name, "pre") == 0 || strcmp(name, "code") == 0) {
                if (strcmp(name, "pre") == 0) {
                    layout_newline(&ly, false);
                    ly.in_pre = !is_close;
                }
                ly.cur_style = is_close ? 0 : 4;
            } else if (strcmp(name, "blockquote") == 0) {
                layout_newline(&ly, false);
                ly.cur_style = is_close ? 0 : 5;
            } else if (strcmp(name, "td") == 0 || strcmp(name, "th") == 0) {
                if (!is_close && ly.cx > 0) {
                    layout_put_utf8(&ly, " | ");
                }
            } else if (strcmp(name, "img") == 0 && !is_close) {
                char src[300] = "";
                char alt[128] = "";
                if (!extract_attr_val(tag_buf, "src", src, sizeof(src))) {
                    extract_attr_val(tag_buf, "data-src", src, sizeof(src));
                }
                extract_attr_val(tag_buf, "alt", alt, sizeof(alt));
                if (src[0] && !str_istarts_with(src, "data:")) {
                    char full_img_url[320] = "";
                    resolve_relative_url(doc->current_url, src, full_img_url, sizeof(full_img_url));

                    /* Convert st-note.com CDN images to resized PNG on the server */
                    if (str_ifind(full_img_url, "st-note.com") != NULL) {
                        char *q = strchr(full_img_url, '?');
                        if (q) *q = 0;
                        size_t ulen = strlen(full_img_url);
                        ksnprintf(full_img_url + ulen, sizeof(full_img_url) - ulen,
                                  "?width=%d&format=png", WEB_IMG_MAX_W);
                    }

                    bool decoded = false;
                    if (doc->image_count < WEB_MAX_IMAGES &&
                        (str_ifind(full_img_url, ".png") != NULL ||
                         str_ifind(full_img_url, "format=png") != NULL)) {
                        uint8_t *img_buf = (uint8_t *)kmalloc_psram(WEB_IMG_BUF_CAP);
                        if (img_buf) {
                            size_t img_bytes = 0;
                            if (progress_cb) {
                                char msg[64];
                                ksnprintf(msg, sizeof(msg), "Loading image %d...", doc->image_count + 1);
                                progress_cb(msg, full_img_url);
                            }
                            if (web_fetch_binary(full_img_url, img_buf, WEB_IMG_BUF_CAP, &img_bytes)) {
                                int iw = 0, ih = 0;
                                uint16_t *px = NULL;
                                if (decode_png_rgb565(img_buf, img_bytes,
                                                      WEB_IMG_MAX_W, WEB_IMG_MAX_H,
                                                      &px, &iw, &ih) &&
                                    px && iw > 0 && ih > 0) {
                                    int iid = doc->image_count++;
                                    doc->images[iid].pixels = px;
                                    doc->images[iid].w      = (int16_t)iw;
                                    doc->images[iid].h      = (int16_t)ih;

                                    layout_newline(&ly, false);
                                    int rows = (ih + WEB_CELL_PX - 1) / WEB_CELL_PX;
                                    for (int r = 0; r < rows && doc->line_count < WEB_MAX_LINES - 2; r++) {
                                        doc->lines[doc->line_count].img_id    = (int16_t)iid;
                                        doc->lines[doc->line_count].img_row_y = (int16_t)(r * WEB_CELL_PX);
                                        doc->line_count++;
                                        layout_init_line(&doc->lines[doc->line_count]);
                                    }
                                    ly.cx = 0;
                                    ly.last_was_space = true;
                                    ly.last_was_blank_line = false;
                                    decoded = true;
                                }
                            }
                            kfree(img_buf);
                        }
                    }
                    if (!decoded && alt[0]) {
                        uint8_t prev = ly.cur_style;
                        ly.cur_style = 2;
                        layout_put_utf8(&ly, "[画像: ");
                        layout_put_utf8(&ly, alt);
                        layout_put_utf8(&ly, "]");
                        ly.cur_style = prev;
                    }
                }
            } else if (strcmp(name, "a") == 0) {
                if (!is_close) {
                    char href[300] = "";
                    if (extract_attr_href(tag_buf, href, sizeof(href)) &&
                        !str_istarts_with(href, "javascript:") &&
                        href[0] != '#' &&
                        doc->link_count < WEB_MAX_LINKS) {
                        int lid = doc->link_count++;
                        resolve_relative_url(doc->current_url, href, doc->links[lid].url, sizeof(doc->links[lid].url));
                        doc->links[lid].line_idx = doc->line_count;
                        ly.cur_link  = (int16_t)lid;
                        ly.cur_style = 3;
                        char badge[16];
                        ksnprintf(badge, sizeof(badge), "[%d]", lid + 1);
                        layout_put_utf8(&ly, badge);
                    }
                } else {
                    ly.cur_link  = -1;
                    ly.cur_style = 0;
                }
            }
            continue;
        }

        if (*p == '&') {
            uint32_t cp = decode_html_entity(&p);
            layout_put_cp(&ly, cp);
            continue;
        }

        const uint8_t *up = (const uint8_t *)p;
        uint8_t b0 = *up++;
        uint32_t cp = b0;
        if ((b0 & 0xE0u) == 0xC0u && (up[0] & 0xC0u) == 0x80u) {
            cp = ((uint32_t)(b0 & 0x1Fu) << 6) | (uint32_t)(up[0] & 0x3Fu);
            up += 1;
        } else if ((b0 & 0xF0u) == 0xE0u && (up[0] & 0xC0u) == 0x80u && (up[1] & 0xC0u) == 0x80u) {
            cp = ((uint32_t)(b0 & 0x0Fu) << 12) | ((uint32_t)(up[0] & 0x3Fu) << 6) | (uint32_t)(up[1] & 0x3Fu);
            up += 2;
        } else if ((b0 & 0xF8u) == 0xF0u && (up[0] & 0xC0u) == 0x80u && (up[1] & 0xC0u) == 0x80u && (up[2] & 0xC0u) == 0x80u) {
            cp = 0xFFFDu;
            up += 3;
        }
        p = (const char *)up;
        layout_put_cp(&ly, cp);
    }

    doc->line_count++;
    if (doc->sel_link >= doc->link_count) {
        doc->sel_link = (doc->link_count > 0) ? 0 : -1;
    }
}

/* =========================================================================
 * 5. Graphical Browser UI & Bookmarks
 * ========================================================================= */

static void gui_draw_padded(int cx, int cy, int width_cells, const char *s, uint16_t fg, uint16_t bg)
{
    uint16_t cps[WEB_COLS];
    int n = 0;
    if (width_cells > WEB_COLS) width_cells = WEB_COLS;
    const uint8_t *p = (const uint8_t *)s;
    while (p && *p && n < width_cells) {
        uint8_t b0 = *p++;
        uint32_t cp = b0;
        if ((b0 & 0xE0u) == 0xC0u && (p[0] & 0xC0u) == 0x80u) {
            cp = ((uint32_t)(b0 & 0x1Fu) << 6) | (uint32_t)(p[0] & 0x3Fu);
            p += 1;
        } else if ((b0 & 0xF0u) == 0xE0u && (p[0] & 0xC0u) == 0x80u && (p[1] & 0xC0u) == 0x80u) {
            cp = ((uint32_t)(b0 & 0x0Fu) << 12) | ((uint32_t)(p[0] & 0x3Fu) << 6) | (uint32_t)(p[1] & 0x3Fu);
            p += 2;
        } else if ((b0 & 0xF8u) == 0xF0u && (p[0] & 0xC0u) == 0x80u && (p[1] & 0xC0u) == 0x80u && (p[2] & 0xC0u) == 0x80u) {
            cp = 0xFFFDu;
            p += 3;
        }
        cps[n++] = (uint16_t)(cp <= 0xFFFFu ? cp : 0xFFFDu);
    }
    while (n < width_cells) cps[n++] = ' ';
    display_draw_codepoints(cx * WEB_CELL_PX, cy * WEB_CELL_PX, cps, width_cells, fg, bg, M5OS_FBCON_SCALE);
}

static void gui_status_popup(const char *line1, const char *line2)
{
    int w = 64, h = 6;
    int x = (WEB_COLS - w) / 2;
    int y = (dq_rows() - h) / 2;
    dq_window(x, y, w, h);
    gui_draw_padded(x + 2, y + 2, w - 4, line1 ? line1 : "", COLOR_YELLOW, COLOR_BLACK);
    gui_draw_padded(x + 2, y + 3, w - 4, line2 ? line2 : "", COLOR_CYAN,   COLOR_BLACK);
}

static void web_render_screen(const web_doc_t *doc, int hist_depth)
{
    /* Row 0: Title Bar */
    char top0[128];
    ksnprintf(top0, sizeof(top0), " MeOS Web | %s", doc->title[0] ? doc->title : "Ready");
    gui_draw_padded(0, 0, 68, top0, RGB565(255, 215, 0), RGB565(18, 28, 56));

    char badge[16];
    ksnprintf(badge, sizeof(badge), " HTTP %3d ", doc->status_code ? doc->status_code : 200);
    gui_draw_padded(68, 0, 12, badge, COLOR_BLACK,
                    (doc->status_code >= 200 && doc->status_code < 400) ? COLOR_GREEN : COLOR_YELLOW);

    /* Row 1: URL Bar + Scroll & Link indicator */
    char url_bar[128];
    ksnprintf(url_bar, sizeof(url_bar), " URL: %-46.46s | Img:%d Lnk:%d/%d Ln:%d/%d",
              doc->current_url,
              doc->image_count,
              (doc->sel_link >= 0) ? (doc->sel_link + 1) : 0,
              doc->link_count,
              doc->scroll_y + 1,
              doc->line_count);
    gui_draw_padded(0, 1, WEB_COLS, url_bar, RGB565(190, 230, 255), RGB565(10, 16, 34));

    /* Rows 2..42: Document Viewport */
    for (int r = 0; r < WEB_VIEW_ROWS; r++) {
        int lidx = doc->scroll_y + r;
        int sy   = (WEB_TOP_ROWS + r) * WEB_CELL_PX;
        if (lidx < 0 || lidx >= doc->line_count) {
            gui_draw_padded(0, WEB_TOP_ROWS + r, WEB_COLS, "", COLOR_WHITE, COLOR_BLACK);
            continue;
        }

        const web_line_t *ln = &doc->lines[lidx];
        if (ln->img_id >= 0 && ln->img_id < doc->image_count) {
            gui_draw_padded(0, WEB_TOP_ROWS + r, WEB_COLS, "", COLOR_WHITE, COLOR_BLACK);
            const web_image_t *im = &doc->images[ln->img_id];
            int y0 = ln->img_row_y;
            int sh = im->h - y0;
            if (sh > WEB_CELL_PX) sh = WEB_CELL_PX;
            if (im->pixels && y0 >= 0 && sh > 0) {
                display_draw_bitmap(2 * WEB_CELL_PX, sy, im->w, sh, im->pixels + (size_t)y0 * im->w);
            }
            continue;
        }

        int x = 0;
        while (x < WEB_COLS) {
            uint8_t st = ln->style[x];
            int16_t lid = ln->link_id[x];
            bool is_sel_link = (lid >= 0 && lid == doc->sel_link);

            uint16_t fg = RGB565(230, 235, 240);
            uint16_t bg = COLOR_BLACK;
            if (is_sel_link) {
                fg = COLOR_BLACK;
                bg = RGB565(255, 215, 0);
            } else if (st == 1) {
                fg = RGB565(255, 225, 60);
            } else if (st == 2) {
                fg = RGB565(80, 235, 150);
            } else if (st == 3) {
                fg = RGB565(60, 210, 255);
            } else if (st == 4) {
                fg = RGB565(200, 255, 200);
                bg = RGB565(16, 22, 32);
            } else if (st == 5) {
                fg = RGB565(160, 175, 195);
            }

            uint16_t run[WEB_COLS];
            int run_len = 0;
            int start_x = x;
            while (x < WEB_COLS) {
                bool sel2 = (ln->link_id[x] >= 0 && ln->link_id[x] == doc->sel_link);
                if (ln->style[x] != st || sel2 != is_sel_link) break;
                run[run_len++] = ln->cp[x];
                x++;
            }
            display_draw_codepoints(start_x * WEB_CELL_PX, sy, run, run_len, fg, bg, M5OS_FBCON_SCALE);
        }
    }

    /* Row 43: Selected Link Target Preview */
    char link_bar[128];
    if (doc->sel_link >= 0 && doc->sel_link < doc->link_count) {
        ksnprintf(link_bar, sizeof(link_bar), " Link [%d]: %s",
                  doc->sel_link + 1, doc->links[doc->sel_link].url);
    } else {
        ksnprintf(link_bar, sizeof(link_bar),
                  " History: %d  |  Mode: %s  |  Size: %u bytes  |  Images: %d",
                  hist_depth, doc->source_mode ? "HTML Source" : "Rendered View",
                  (unsigned)doc->raw_bytes, doc->image_count);
    }
    gui_draw_padded(0, 43, WEB_COLS, link_bar, COLOR_YELLOW, RGB565(18, 28, 56));

    /* Row 44: Keybindings Help Bar */
    gui_draw_padded(0, 44, WEB_COLS,
                    " [Up/Dn/Spc] Scroll [Tab/n/p] Link [Enter] Open [G] URL [S] Search [B] Back [M] Bookmarks [Q] Quit",
                    COLOR_CYAN, RGB565(12, 18, 38));
}

static void url_encode_query(const char *q, char *out, size_t out_cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t i = 0;
    for (const uint8_t *p = (const uint8_t *)q; *p && i + 4 < out_cap; p++) {
        uint8_t c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') {
            out[i++] = (char)c;
        } else if (c == ' ') {
            out[i++] = '+';
        } else {
            out[i++] = '%';
            out[i++] = hex[c >> 4];
            out[i++] = hex[c & 15u];
        }
    }
    out[i] = 0;
}

static bool is_likely_url(const char *s)
{
    while (*s == ' ') s++;
    if (!*s) return false;
    if (str_istarts_with(s, "http://") || str_istarts_with(s, "https://")) {
        return true;
    }
    bool has_dot = false;
    bool has_slash_or_colon = false;
    size_t len = 0;
    for (const char *p = s; *p; p++, len++) {
        uint8_t c = (uint8_t)*p;
        /* Any space, control char, or non-ASCII UTF-8 byte means search keywords */
        if (c <= ' ' || c >= 0x7Fu) return false;
        if (c == '.') has_dot = true;
        if (c == '/' || c == ':') has_slash_or_colon = true;
    }
    if (has_slash_or_colon) return true;
    if (has_dot && len >= 3 && s[0] != '.' && s[len - 1] != '.') return true;
    return false;
}

static void web_resolve_input_or_search(const char *input, char *out_url, size_t out_cap)
{
    while (*input == ' ') input++;
    if (!*input) {
        strncpy(out_url, "https://lite.duckduckgo.com/lite/", out_cap - 1);
        out_url[out_cap - 1] = 0;
        return;
    }
    if (is_likely_url(input)) {
        strncpy(out_url, input, out_cap - 1);
        out_url[out_cap - 1] = 0;
    } else {
        char enc[240];
        url_encode_query(input, enc, sizeof(enc));
        ksnprintf(out_url, out_cap, "https://lite.duckduckgo.com/lite/?q=%s", enc);
    }
}

static void join_argv_space(int start_idx, int argc, char **argv, char *out, size_t out_cap)
{
    size_t pos = 0;
    for (int i = start_idx; i < argc && pos + 1 < out_cap; i++) {
        if (i > start_idx && pos + 1 < out_cap) {
            out[pos++] = ' ';
        }
        for (const char *p = argv[i]; *p && pos + 1 < out_cap; p++) {
            out[pos++] = *p;
        }
    }
    out[pos] = 0;
}

static bool ensure_wifi_connected(void)
{
    wifi_status_t st;
    wifi_get_status(&st);
    if (st.state == WIFI_STATE_CONNECTED && st.ip[0] != 0) return true;

    char ssid[WIFI_SSID_MAX + 1] = "";
    char pass[WIFI_PASS_MAX + 1] = "";
    bool auto_conn = false;
    if (st.state != WIFI_STATE_CONNECTED &&
        wifi_load_saved(ssid, pass, &auto_conn) && auto_conn && ssid[0]) {
        wifi_connect(ssid, pass, false);
    }
    for (int i = 0; i < 40; i++) {
        wifi_get_status(&st);
        if (st.state == WIFI_STATE_CONNECTED && st.ip[0] != 0) return true;
        task_sleep(100);
    }
    return st.state == WIFI_STATE_CONNECTED && st.ip[0] != 0;
}

static bool bookmarks_dialog(char *chosen_url, size_t chosen_cap)
{
    static const struct { const char *title; const char *url; } presets[] = {
        { "1. Example.com (HTTPS Test)",           "https://example.com" },
        { "2. DuckDuckGo Lite (Web Search)",       "https://lite.duckduckgo.com/lite/" },
        { "3. shi3z note (note.com/shi3zblog)",    "https://note.com/shi3zblog" },
        { "4. Hacker News (news.ycombinator.com)", "https://news.ycombinator.com" },
        { "5. Wikipedia 日本語 (メインページ)",       "https://ja.m.wikipedia.org/wiki/%E3%83%A1%E3%82%A4%E3%83%B3%E3%83%9A%E3%83%BC%E3%82%B8" },
        { "6. Tailscale tsuginosuke (:8000)",      "http://tsuginosuke:8000" },
    };
    const int count = (int)(sizeof(presets) / sizeof(presets[0]));
    int sel = 0;

    for (;;) {
        int w = 72, h = 20;
        int x = (WEB_COLS - w) / 2;
        int y = (dq_rows() - h) / 2;
        dq_window(x, y, w, h);
        gui_draw_padded(x + 2, y + 1, w - 4, "BOOKMARKS & QUICK START SITES", RGB565(255, 215, 0), COLOR_BLACK);
        for (int i = 0; i < count; i++) {
            uint16_t fg = (i == sel) ? COLOR_BLACK : COLOR_WHITE;
            uint16_t bg = (i == sel) ? RGB565(255, 215, 0) : COLOR_BLACK;
            char line[80];
            ksnprintf(line, sizeof(line), " %s", presets[i].title);
            gui_draw_padded(x + 2, y + 3 + i * 2, w - 4, line, fg, bg);
            gui_draw_padded(x + 4, y + 4 + i * 2, w - 6, presets[i].url,
                            (i == sel) ? RGB565(180, 240, 255) : RGB565(130, 150, 175), COLOR_BLACK);
        }
        gui_draw_padded(x + 2, y + h - 2, w - 4, "[Up/Dn] Select   [Enter] Open Site   [Esc] Cancel", COLOR_CYAN, COLOR_BLACK);

        int k = dq_getkey();
        if (k == DQK_UP || k == 'k') {
            if (sel > 0) sel--;
        } else if (k == DQK_DOWN || k == 'j') {
            if (sel + 1 < count) sel++;
        } else if (k >= '1' && k < '1' + count) {
            sel = k - '1';
            strncpy(chosen_url, presets[sel].url, chosen_cap - 1);
            chosen_url[chosen_cap - 1] = 0;
            return true;
        } else if (k == DQK_ENTER) {
            strncpy(chosen_url, presets[sel].url, chosen_cap - 1);
            chosen_url[chosen_cap - 1] = 0;
            return true;
        } else if (k == DQK_ESC || k == 'q' || k == 'm') {
            return false;
        }
    }
}

void web_app_open_url(const char *initial_url)
{
    web_doc_t *doc = (web_doc_t *)kmalloc_psram(sizeof(web_doc_t));
    if (!doc) doc = (web_doc_t *)kmalloc(sizeof(web_doc_t));
    if (!doc) {
        kprintf("web: out of memory\n");
        return;
    }
    memset(doc, 0, sizeof(*doc));

    doc->raw_html = (char *)kmalloc_psram(WEB_HTML_BUF_CAP);
    doc->lines    = (web_line_t *)kmalloc_psram(sizeof(web_line_t) * WEB_MAX_LINES);
    doc->links    = (web_link_t *)kmalloc_psram(sizeof(web_link_t) * WEB_MAX_LINKS);
    char (*hist)[320] = (char (*)[320])kmalloc_psram(320 * WEB_MAX_HIST);

    if (!doc->raw_html || !doc->lines || !doc->links || !hist) {
        if (doc->raw_html) kfree(doc->raw_html);
        if (doc->lines)    kfree(doc->lines);
        if (doc->links)    kfree(doc->links);
        if (hist)          kfree(hist);
        kfree(doc);
        kprintf("web: out of PSRAM\n");
        return;
    }

    int hist_top = 0;
    if (initial_url && initial_url[0]) {
        web_resolve_input_or_search(initial_url, doc->current_url, sizeof(doc->current_url));
    } else {
        strncpy(doc->current_url, "https://example.com", sizeof(doc->current_url) - 1);
    }

    dq_clear();

    bool need_fetch  = true;
    bool need_redraw = true;

    for (;;) {
        if (need_fetch) {
            need_fetch = false;
            gui_status_popup("Checking Wi-Fi connection...", doc->current_url);
            if (!ensure_wifi_connected()) {
                dq_message("Wi-Fi is not connected.", "Press [W] to configure Wi-Fi.");
                strcpy(doc->raw_html,
                       "<html><head><title>Offline</title></head><body>"
                       "<h1>Wi-Fi が未接続です</h1>"
                       "<p>キーボードの [W] を押して Wi-Fi に接続するか、[G] でURLを入力してください。</p>"
                       "</body></html>");
                doc->raw_bytes = strlen(doc->raw_html);
                doc->status_code = 0;
                web_render_document(doc, gui_status_popup);
            } else {
                char err[128] = "";
                char resolved[320] = "";
                size_t got_len = 0;
                int st_code = 0;
                bool ok = note_try_fetch_clean_html(doc->current_url, resolved, sizeof(resolved),
                                                    doc->raw_html, WEB_HTML_BUF_CAP, &got_len,
                                                    &st_code, err, sizeof(err), gui_status_popup);
                if (!ok) {
                    ok = web_fetch_url(doc->current_url, resolved, sizeof(resolved),
                                       doc->raw_html, WEB_HTML_BUF_CAP, &got_len,
                                       &st_code, err, sizeof(err), gui_status_popup);
                }
                if (ok) {
                    strncpy(doc->current_url, resolved, sizeof(doc->current_url) - 1);
                    doc->raw_bytes   = got_len;
                    doc->status_code = st_code;
                    doc->scroll_y    = 0;
                    doc->sel_link    = 0;
                    web_render_document(doc, gui_status_popup);
                } else {
                    ksnprintf(doc->raw_html, WEB_HTML_BUF_CAP,
                              "<html><head><title>Load Error</title></head><body>"
                              "<h1>ページを読み込めませんでした</h1>"
                              "<p>URL: %s</p>"
                              "<p>Error: %s</p>"
                              "<p><a href=\"https://example.com\">Example.com を開く</a></p>"
                              "<p><a href=\"https://lite.duckduckgo.com/lite/\">DuckDuckGo 検索を開く</a></p>"
                              "</body></html>",
                              doc->current_url, err);
                    doc->raw_bytes   = strlen(doc->raw_html);
                    doc->status_code = 0;
                    doc->scroll_y    = 0;
                    doc->sel_link    = 0;
                    web_render_document(doc, gui_status_popup);
                }
            }
            need_redraw = true;
        }

        if (need_redraw) {
            web_render_screen(doc, hist_top);
            need_redraw = false;
        }

        int k = dq_getkey();
        if (k == DQK_REFRESH) { need_redraw = true; }                  /* the on-screen keyboard covered part of the page */
        else if (k == DQK_UP || k == 'k') {
            if (doc->scroll_y > 0) { doc->scroll_y--; need_redraw = true; }
        } else if (k == DQK_DOWN || k == 'j') {
            if (doc->scroll_y + 1 < doc->line_count) { doc->scroll_y++; need_redraw = true; }
        } else if (k == DQK_PGDN || k == ' ' || k == 0x04) {
            int max_s = doc->line_count - WEB_VIEW_ROWS;
            if (max_s < 0) max_s = 0;
            doc->scroll_y += (WEB_VIEW_ROWS - 3);
            if (doc->scroll_y > max_s) doc->scroll_y = max_s;
            need_redraw = true;
        } else if (k == DQK_PGUP || k == 0x15) {
            doc->scroll_y -= (WEB_VIEW_ROWS - 3);
            if (doc->scroll_y < 0) doc->scroll_y = 0;
            need_redraw = true;
        } else if (k == DQK_HOME) {
            doc->scroll_y = 0;
            need_redraw = true;
        } else if (k == DQK_END) {
            int max_s = doc->line_count - WEB_VIEW_ROWS;
            doc->scroll_y = (max_s > 0) ? max_s : 0;
            need_redraw = true;
        } else if (k == '\t' || k == 'n' || k == DQK_RIGHT || k == ']') {
            if (doc->link_count > 0) {
                doc->sel_link = (doc->sel_link + 1) % doc->link_count;
                int lrow = doc->links[doc->sel_link].line_idx;
                if (lrow < doc->scroll_y || lrow >= doc->scroll_y + WEB_VIEW_ROWS) {
                    doc->scroll_y = (lrow > 3) ? (lrow - 3) : 0;
                }
                need_redraw = true;
            }
        } else if (k == 'p' || k == DQK_LEFT || k == '[') {
            if (doc->link_count > 0) {
                doc->sel_link = (doc->sel_link + doc->link_count - 1) % doc->link_count;
                int lrow = doc->links[doc->sel_link].line_idx;
                if (lrow < doc->scroll_y || lrow >= doc->scroll_y + WEB_VIEW_ROWS) {
                    doc->scroll_y = (lrow > 3) ? (lrow - 3) : 0;
                }
                need_redraw = true;
            }
        } else if (k == DQK_ENTER) {
            if (doc->sel_link >= 0 && doc->sel_link < doc->link_count) {
                if (hist_top < WEB_MAX_HIST) {
                    strncpy(hist[hist_top++], doc->current_url, 319);
                }
                strncpy(doc->current_url, doc->links[doc->sel_link].url, sizeof(doc->current_url) - 1);
                need_fetch = true;
            }
        } else if (k == 'b' || k == 'B' || k == DQK_BACKSPACE) {
            if (hist_top > 0) {
                strncpy(doc->current_url, hist[--hist_top], sizeof(doc->current_url) - 1);
                need_fetch = true;
            }
        } else if (k == 'r' || k == 'R') {
            need_fetch = true;
        } else if (k == 'v' || k == 'V') {
            doc->source_mode = !doc->source_mode;
            doc->scroll_y = 0;
            web_render_document(doc, gui_status_popup);
            need_redraw = true;
        } else if (k == 'g' || k == 'G' || k == 'u' || k == 'U' || k == 0x0C) {
            char new_url[320];
            strncpy(new_url, doc->current_url, sizeof(new_url) - 1);
            new_url[sizeof(new_url) - 1] = 0;
            if (dq_input("Enter URL or Search Keywords", new_url, sizeof(new_url)) && new_url[0]) {
                if (hist_top < WEB_MAX_HIST) {
                    strncpy(hist[hist_top++], doc->current_url, 319);
                }
                web_resolve_input_or_search(new_url, doc->current_url, sizeof(doc->current_url));
                need_fetch = true;
            } else {
                need_redraw = true;
            }
        } else if (k == 's' || k == 'S' || k == '/') {
            char query[128] = "";
            if (dq_input("Web Search (DuckDuckGo Lite)", query, sizeof(query)) && query[0]) {
                char enc[240];
                url_encode_query(query, enc, sizeof(enc));
                if (hist_top < WEB_MAX_HIST) {
                    strncpy(hist[hist_top++], doc->current_url, 319);
                }
                ksnprintf(doc->current_url, sizeof(doc->current_url),
                          "https://lite.duckduckgo.com/lite/?q=%s", enc);
                need_fetch = true;
            } else {
                need_redraw = true;
            }
        } else if (k == 'm' || k == 'M') {
            char chosen[320] = "";
            if (bookmarks_dialog(chosen, sizeof(chosen)) && chosen[0]) {
                if (hist_top < WEB_MAX_HIST) {
                    strncpy(hist[hist_top++], doc->current_url, 319);
                }
                strncpy(doc->current_url, chosen, sizeof(doc->current_url) - 1);
                need_fetch = true;
            } else {
                need_redraw = true;
            }
        } else if (k == 'w' || k == 'W') {
            wifi_app_run();
            need_redraw = true;
        } else if (k == 'q' || k == 'Q' || k == DQK_ESC) {
            break;
        }
    }

    web_free_images(doc);
    kfree(hist);
    kfree(doc->links);
    kfree(doc->lines);
    kfree(doc->raw_html);
    kfree(doc);
    dq_clear();
}

void web_app_run(void)
{
    web_app_open_url("https://example.com");
}

static void print_codepoint_utf8(uint16_t cp)
{
    char out[4];
    if (cp < 0x80u) {
        out[0] = (char)cp;
        kconsole_write(out, 1);
    } else if (cp < 0x800u) {
        out[0] = (char)(0xC0u | (cp >> 6));
        out[1] = (char)(0x80u | (cp & 0x3Fu));
        kconsole_write(out, 2);
    } else {
        out[0] = (char)(0xE0u | (cp >> 12));
        out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (cp & 0x3Fu));
        kconsole_write(out, 3);
    }
}

int web_cmd_main(int argc, char **argv)
{
    if (argc >= 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        kprintf("Usage:\n");
        kprintf("  web                         Open graphical Web Browser\n");
        kprintf("  web <keywords...>           Search keywords on DuckDuckGo in Browser\n");
        kprintf("  web <url>                   Open URL in graphical Web Browser\n");
        kprintf("  web --dump <url|keywords>   Fetch/search and render page to terminal\n");
        return 0;
    }

    if (argc >= 3 && strcmp(argv[1], "--dump") == 0) {
        if (!ensure_wifi_connected()) {
            kprintf("web: Wi-Fi is not connected\n");
            return 1;
        }
        web_doc_t *doc = (web_doc_t *)kmalloc_psram(sizeof(web_doc_t));
        if (!doc) return 1;
        memset(doc, 0, sizeof(*doc));
        doc->raw_html = (char *)kmalloc_psram(WEB_HTML_BUF_CAP);
        doc->lines    = (web_line_t *)kmalloc_psram(sizeof(web_line_t) * WEB_MAX_LINES);
        doc->links    = (web_link_t *)kmalloc_psram(sizeof(web_link_t) * WEB_MAX_LINKS);
        if (!doc->raw_html || !doc->lines || !doc->links) {
            if (doc->raw_html) kfree(doc->raw_html);
            if (doc->lines)    kfree(doc->lines);
            if (doc->links)    kfree(doc->links);
            kfree(doc);
            return 1;
        }

        char combined[256];
        char target_url[320];
        join_argv_space(2, argc, argv, combined, sizeof(combined));
        web_resolve_input_or_search(combined, target_url, sizeof(target_url));

        char err[128] = "";
        size_t got_len = 0;
        int st_code = 0;
        kprintf("web: fetching %s ...\n", target_url);
        bool ok = note_try_fetch_clean_html(target_url, doc->current_url, sizeof(doc->current_url),
                                            doc->raw_html, WEB_HTML_BUF_CAP, &got_len,
                                            &st_code, err, sizeof(err), NULL);
        if (!ok) {
            ok = web_fetch_url(target_url, doc->current_url, sizeof(doc->current_url),
                               doc->raw_html, WEB_HTML_BUF_CAP, &got_len, &st_code, err, sizeof(err), NULL);
        }
        if (!ok) {
            kprintf("web: %s\n", err);
            kfree(doc->links);
            kfree(doc->lines);
            kfree(doc->raw_html);
            kfree(doc);
            return 1;
        }
        doc->raw_bytes   = got_len;
        doc->status_code = st_code;
        web_render_document(doc, NULL);

        kprintf("=== %s (HTTP %d, %u bytes, %d links, %d images) ===\n",
                doc->title, doc->status_code, (unsigned)doc->raw_bytes,
                doc->link_count, doc->image_count);
        for (int r = 0; r < doc->line_count; r++) {
            if (doc->lines[r].img_id >= 0 && doc->lines[r].img_row_y == 0) {
                int iid = doc->lines[r].img_id;
                kprintf("  [IMAGE #%d: %dx%d px RGB565]\n",
                        iid + 1, doc->images[iid].w, doc->images[iid].h);
                continue;
            } else if (doc->lines[r].img_id >= 0) {
                continue;
            }
            int last_non_space = -1;
            for (int c = 0; c < WEB_COLS; c++) {
                if (doc->lines[r].cp[c] != ' ') last_non_space = c;
            }
            for (int c = 0; c <= last_non_space; c++) {
                print_codepoint_utf8(doc->lines[r].cp[c]);
            }
            kprintf("\n");
        }

        web_free_images(doc);
        kfree(doc->links);
        kfree(doc->lines);
        kfree(doc->raw_html);
        kfree(doc);
        return 0;
    }

    if (argc >= 2) {
        char combined[256];
        join_argv_space(1, argc, argv, combined, sizeof(combined));
        web_app_open_url(combined);
        return 0;
    }

    web_app_run();
    return 0;
}

void web_main(void *arg)
{
    (void)arg;
    web_app_run();
}


/* ---- MeOS app installer: download a file ------------------------------------------------------ */

/* Downloads `url` (http or https, redirects followed) into buf, body only.  Used by user/wasmapps/qrinstall.c. */
bool web_download(const char *url, uint8_t *buf, size_t cap, size_t *out_len, char *err, size_t err_cap)
{
    char final_url[320];
    int status = 0;
    err[0] = 0;
    if (!web_fetch_url(url, final_url, sizeof final_url, (char *)buf, cap, out_len, &status, err, err_cap, NULL)) {
        if (!err[0]) ksnprintf(err, err_cap, "download failed");
        return false;
    }
    if (status != 200) { ksnprintf(err, err_cap, "HTTP %d", status); return false; }
    return true;
}
