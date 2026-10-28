/*
 * M5OS SSH-2 Client (RFC 4253 / 4252 / 4254 / 8731)
 *
 * Self-contained SSH-2 implementation for M5Stack Tab5:
 *  - Transport  : SSH-2.0 binary packet protocol over M5OS TCP/IP + DNS
 *  - KEX        : curve25519-sha256, curve25519-sha256@libssh.org (X25519 + SHA-256)
 *  - Host Key   : ssh-ed25519, ecdsa-sha2-nistp256, rsa-sha2-256, rsa-sha2-512, ssh-rsa
 *  - Cipher     : aes128-ctr
 *  - MAC        : hmac-sha2-256
 *  - Userauth   : password, keyboard-interactive (PAM)
 *  - Connection : interactive PTY shell (vt100 80x45) or single-command exec
 */
#include "ssh_app.h"
#include "../tui/dqwin.h"
#include "../wifi/wifi_app.h"
#include "../../drivers/lcd/fbcon.h"
#include <m5os/wifi.h>
#include <m5os/display.h>
#include <m5os/vfs.h>
#include <m5os/kmalloc.h>
#include <m5os/kprintf.h>
#include <m5os/hal_time.h>
#include <m5os/task.h>
#include <m5os/arch.h>
#include <m5os/crc32.h>
#include <m5os/errno.h>
#include <string.h>
#if M5OS_BOARD_TAB5
#include "esp_attr.h"
#include "esp_rom_spiflash.h"
#endif

#define SSH_CFG_PATH       "/home/ssh.cfg"
#define SSH_HOSTS_CFG_PATH "/home/ssh_hosts.cfg"
#define SSH_KEY_PATH       "/home/id_ed25519"
#define SSH_PUBKEY_PATH    "/home/id_ed25519.pub"
#define SSH_FLASH_KEY_ADDR 0x003F0000u
#define SSH_BANNER_CLIENT  "SSH-2.0-MeOS_1.0"
#define SSH_MAX_PKT        4608

/* SSH-2 Message Numbers (RFC 4253 / 4252 / 4254) */
#define SSH_MSG_DISCONNECT                1
#define SSH_MSG_IGNORE                    2
#define SSH_MSG_UNIMPLEMENTED             3
#define SSH_MSG_DEBUG                     4
#define SSH_MSG_SERVICE_REQUEST           5
#define SSH_MSG_SERVICE_ACCEPT            6
#define SSH_MSG_EXT_INFO                  7
#define SSH_MSG_KEXINIT                   20
#define SSH_MSG_NEWKEYS                   21
#define SSH_MSG_KEX_ECDH_INIT             30
#define SSH_MSG_KEX_ECDH_REPLY            31
#define SSH_MSG_USERAUTH_REQUEST          50
#define SSH_MSG_USERAUTH_FAILURE          51
#define SSH_MSG_USERAUTH_SUCCESS          52
#define SSH_MSG_USERAUTH_BANNER           53
#define SSH_MSG_USERAUTH_INFO_REQUEST     60
#define SSH_MSG_USERAUTH_INFO_RESPONSE    61
#define SSH_MSG_GLOBAL_REQUEST            80
#define SSH_MSG_REQUEST_SUCCESS           81
#define SSH_MSG_REQUEST_FAILURE           82
#define SSH_MSG_CHANNEL_OPEN              90
#define SSH_MSG_CHANNEL_OPEN_CONFIRMATION 91
#define SSH_MSG_CHANNEL_OPEN_FAILURE      92
#define SSH_MSG_CHANNEL_WINDOW_ADJUST     93
#define SSH_MSG_CHANNEL_DATA              94
#define SSH_MSG_CHANNEL_EXTENDED_DATA     95
#define SSH_MSG_CHANNEL_EOF               96
#define SSH_MSG_CHANNEL_CLOSE             97
#define SSH_MSG_CHANNEL_REQUEST           98
#define SSH_MSG_CHANNEL_SUCCESS           99
#define SSH_MSG_CHANNEL_FAILURE           100

/* =========================================================================
 * 1. Cryptographic Primitives: SHA-256, HMAC-SHA-256, X25519, AES-128-CTR
 * ========================================================================= */

typedef struct {
    uint32_t state[8];
    uint64_t bitcount;
    uint8_t  buf[64];
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

static inline uint32_t rotr32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha256_transform(uint32_t state[8], const uint8_t block[64])
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + S1 + ch + K256[i] + w[i];
        uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = S0 + maj;
        h = g; g = f; f = e; e = d + temp1;
        d = c; c = b; b = a; a = temp1 + temp2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

static void sha256_init(sha256_ctx_t *ctx)
{
    ctx->state[0] = 0x6a09e667u; ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u; ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu; ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu; ctx->state[7] = 0x5be0cd19u;
    ctx->bitcount = 0;
}

static void sha256_update(sha256_ctx_t *ctx, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t idx = (size_t)((ctx->bitcount >> 3) & 0x3Fu);
    ctx->bitcount += (uint64_t)len << 3;
    for (size_t i = 0; i < len; i++) {
        ctx->buf[idx++] = p[i];
        if (idx == 64) {
            sha256_transform(ctx->state, ctx->buf);
            idx = 0;
        }
    }
}

static void sha256_final(sha256_ctx_t *ctx, uint8_t out[32])
{
    size_t idx = (size_t)((ctx->bitcount >> 3) & 0x3Fu);
    ctx->buf[idx++] = 0x80u;
    if (idx > 56) {
        while (idx < 64) ctx->buf[idx++] = 0;
        sha256_transform(ctx->state, ctx->buf);
        idx = 0;
    }
    while (idx < 56) ctx->buf[idx++] = 0;
    for (int i = 0; i < 8; i++) {
        ctx->buf[56 + i] = (uint8_t)(ctx->bitcount >> (56 - i * 8));
    }
    sha256_transform(ctx->state, ctx->buf);
    for (int i = 0; i < 8; i++) {
        out[i * 4]     = (uint8_t)(ctx->state[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(ctx->state[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(ctx->state[i]);
    }
}

static void hmac_sha256_packet(const uint8_t key[32], uint32_t seq,
                               const uint8_t *pkt, size_t pkt_len, uint8_t out[32])
{
    uint8_t k_ipad[64], k_opad[64], seq_be[4], inner[32];
    for (int i = 0; i < 32; i++) {
        k_ipad[i] = key[i] ^ 0x36u;
        k_opad[i] = key[i] ^ 0x5cu;
    }
    for (int i = 32; i < 64; i++) {
        k_ipad[i] = 0x36u;
        k_opad[i] = 0x5cu;
    }
    seq_be[0] = (uint8_t)(seq >> 24);
    seq_be[1] = (uint8_t)(seq >> 16);
    seq_be[2] = (uint8_t)(seq >> 8);
    seq_be[3] = (uint8_t)(seq);

    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, k_ipad, 64);
    sha256_update(&ctx, seq_be, 4);
    sha256_update(&ctx, pkt, pkt_len);
    sha256_final(&ctx, inner);

    sha256_init(&ctx);
    sha256_update(&ctx, k_opad, 64);
    sha256_update(&ctx, inner, 32);
    sha256_final(&ctx, out);
}

/* --- X25519 (RFC 7748) --- */
typedef int64_t gf[16];

static void car25519(gf o)
{
    for (int i = 0; i < 16; i++) {
        o[i] += (1LL << 16);
        int64_t c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c << 16;
    }
}

static void sel25519(gf p, gf q, int b)
{
    int64_t c = ~(b - 1);
    for (int i = 0; i < 16; i++) {
        int64_t t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

static void pack25519(uint8_t *o, const gf n)
{
    gf m, t;
    for (int i = 0; i < 16; i++) t[i] = n[i];
    car25519(t); car25519(t); car25519(t);
    for (int j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        int b = (int)((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        sel25519(t, m, 1 - b);
    }
    for (int i = 0; i < 16; i++) {
        o[2 * i]     = (uint8_t)(t[i] & 0xff);
        o[2 * i + 1] = (uint8_t)(t[i] >> 8);
    }
}

static void unpack25519(gf o, const uint8_t *n)
{
    for (int i = 0; i < 16; i++) o[i] = n[2 * i] + ((int64_t)n[2 * i + 1] << 8);
    o[15] &= 0x7fff;
}

static void A25519(gf o, const gf a, const gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] + b[i]; }
static void Z25519(gf o, const gf a, const gf b) { for (int i = 0; i < 16; i++) o[i] = a[i] - b[i]; }

static void M25519(gf o, const gf a, const gf b)
{
    int64_t t[31];
    for (int i = 0; i < 31; i++) t[i] = 0;
    for (int i = 0; i < 16; i++)
        for (int j = 0; j < 16; j++)
            t[i + j] += a[i] * b[j];
    for (int i = 0; i < 15; i++) t[i] += 38 * t[i + 16];
    for (int i = 0; i < 16; i++) o[i] = t[i];
    car25519(o); car25519(o);
}

static void S25519(gf o, const gf a) { M25519(o, a, a); }

static void inv25519(gf o, const gf i)
{
    gf c;
    for (int a = 0; a < 16; a++) c[a] = i[a];
    for (int a = 253; a >= 0; a--) {
        S25519(c, c);
        if (a != 2 && a != 4) M25519(c, c, i);
    }
    for (int a = 0; a < 16; a++) o[a] = c[a];
}

static void x25519_scalarmult(uint8_t *q, const uint8_t *n, const uint8_t *p)
{
    uint8_t z[32];
    gf x, a, b, c, d, e, f;
    static const gf _121665 = {0xDB41, 1};
    for (int i = 0; i < 31; i++) z[i] = n[i];
    z[31] = (n[31] & 127) | 64;
    z[0] &= 248;
    unpack25519(x, p);
    for (int i = 0; i < 16; i++) { b[i] = x[i]; d[i] = a[i] = c[i] = 0; }
    a[0] = d[0] = 1;
    for (int i = 254; i >= 0; --i) {
        int r = (z[i >> 3] >> (i & 7)) & 1;
        sel25519(a, b, r);
        sel25519(c, d, r);
        A25519(e, a, c);
        Z25519(a, a, c);
        A25519(c, b, d);
        Z25519(b, b, d);
        S25519(d, e);
        S25519(f, a);
        M25519(a, c, a);
        M25519(c, b, e);
        A25519(e, a, c);
        Z25519(a, a, c);
        S25519(b, a);
        Z25519(c, d, f);
        M25519(a, c, _121665);
        A25519(a, a, d);
        M25519(c, c, a);
        M25519(a, d, f);
        M25519(d, b, x);
        S25519(b, e);
        sel25519(a, b, r);
        sel25519(c, d, r);
    }
    inv25519(c, c);
    M25519(a, a, c);
    pack25519(q, a);
}

/* --- SHA-512 & Ed25519 (RFC 8032 / RFC 8709 ssh-ed25519) --- */
typedef struct {
    uint64_t state[8];
    uint64_t count[2];
    uint8_t  buf[128];
} sha512_ctx_t;

static const uint64_t K512[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL
};

static inline uint64_t rotr64(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

static void sha512_transform(uint64_t state[8], const uint8_t block[128])
{
    uint64_t w[80];
    for (int i = 0; i < 16; i++) {
        uint64_t v = 0;
        for (int j = 0; j < 8; j++) v = (v << 8) | block[i * 8 + j];
        w[i] = v;
    }
    for (int i = 16; i < 80; i++) {
        uint64_t s0 = rotr64(w[i - 15], 1) ^ rotr64(w[i - 15], 8) ^ (w[i - 15] >> 7);
        uint64_t s1 = rotr64(w[i - 2], 19) ^ rotr64(w[i - 2], 61) ^ (w[i - 2] >> 6);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint64_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint64_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (int i = 0; i < 80; i++) {
        uint64_t S1 = rotr64(e, 14) ^ rotr64(e, 18) ^ rotr64(e, 41);
        uint64_t ch = (e & f) ^ ((~e) & g);
        uint64_t t1 = h + S1 + ch + K512[i] + w[i];
        uint64_t S0 = rotr64(a, 28) ^ rotr64(a, 34) ^ rotr64(a, 39);
        uint64_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint64_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

static void sha512_init(sha512_ctx_t *ctx)
{
    ctx->state[0] = 0x6a09e667f3bcc908ULL; ctx->state[1] = 0xbb67ae8584caa73bULL;
    ctx->state[2] = 0x3c6ef372fe94f82bULL; ctx->state[3] = 0xa54ff53a5f1d36f1ULL;
    ctx->state[4] = 0x510e527fade682d1ULL; ctx->state[5] = 0x9b05688c2b3e6c1fULL;
    ctx->state[6] = 0x1f83d9abfb41bd6bULL; ctx->state[7] = 0x5be0cd19137e2179ULL;
    ctx->count[0] = 0; ctx->count[1] = 0;
}

static void sha512_update(sha512_ctx_t *ctx, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t idx = (size_t)((ctx->count[0] >> 3) & 0x7Fu);
    uint64_t bits = (uint64_t)len << 3;
    ctx->count[0] += bits;
    if (ctx->count[0] < bits) ctx->count[1]++;
    ctx->count[1] += (uint64_t)len >> 61;
    for (size_t i = 0; i < len; i++) {
        ctx->buf[idx++] = p[i];
        if (idx == 128) {
            sha512_transform(ctx->state, ctx->buf);
            idx = 0;
        }
    }
}

static void sha512_final(sha512_ctx_t *ctx, uint8_t out[64])
{
    size_t idx = (size_t)((ctx->count[0] >> 3) & 0x7Fu);
    ctx->buf[idx++] = 0x80u;
    if (idx > 112) {
        while (idx < 128) ctx->buf[idx++] = 0;
        sha512_transform(ctx->state, ctx->buf);
        idx = 0;
    }
    while (idx < 112) ctx->buf[idx++] = 0;
    for (int i = 0; i < 8; i++) {
        ctx->buf[112 + i] = (uint8_t)(ctx->count[1] >> (56 - i * 8));
        ctx->buf[120 + i] = (uint8_t)(ctx->count[0] >> (56 - i * 8));
    }
    sha512_transform(ctx->state, ctx->buf);
    for (int i = 0; i < 8; i++) {
        for (int j = 0; j < 8; j++) {
            out[i * 8 + j] = (uint8_t)(ctx->state[i] >> (56 - j * 8));
        }
    }
}

static const gf ed_gf0 = {0};
static const gf ed_gf1 = {1};
static const gf ed_D2 = {0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283, 0x149a, 0x00e0,
                         0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406};
static const gf ed_X  = {0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525, 0xc760, 0x692c,
                         0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169};
static const gf ed_Y  = {0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
                         0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666};
static const uint64_t ed_L[32] = {
    0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58,
    0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10
};

static uint8_t par25519(const gf a)
{
    uint8_t d[32];
    pack25519(d, a);
    return d[0] & 1u;
}

static void set25519(gf r, const gf a)
{
    for (int i = 0; i < 16; i++) r[i] = a[i];
}

static void ed_cswap(gf p[4], gf q[4], uint8_t b)
{
    for (int i = 0; i < 4; i++) sel25519(p[i], q[i], b);
}

static void ed_add(gf p[4], gf q[4])
{
    gf a, b, c, d, t, e, f, g, h;
    Z25519(a, p[1], p[0]);
    Z25519(t, q[1], q[0]);
    M25519(a, a, t);
    A25519(b, p[0], p[1]);
    A25519(t, q[0], q[1]);
    M25519(b, b, t);
    M25519(c, p[3], q[3]);
    M25519(c, c, ed_D2);
    M25519(d, p[2], q[2]);
    A25519(d, d, d);
    Z25519(e, b, a);
    Z25519(f, d, c);
    A25519(g, d, c);
    A25519(h, b, a);

    M25519(p[0], e, f);
    M25519(p[1], h, g);
    M25519(p[2], g, f);
    M25519(p[3], e, h);
}

static void ed_pack(uint8_t *r, gf p[4])
{
    gf tx, ty, zi;
    inv25519(zi, p[2]);
    M25519(tx, p[0], zi);
    M25519(ty, p[1], zi);
    pack25519(r, ty);
    r[31] ^= (uint8_t)(par25519(tx) << 7);
}

static void ed_scalarmult(gf p[4], gf q[4], const uint8_t *s)
{
    set25519(p[0], ed_gf0);
    set25519(p[1], ed_gf1);
    set25519(p[2], ed_gf1);
    set25519(p[3], ed_gf0);
    for (int i = 255; i >= 0; --i) {
        uint8_t b = (s[i / 8] >> (i & 7)) & 1u;
        ed_cswap(p, q, b);
        ed_add(q, p);
        ed_add(p, p);
        ed_cswap(p, q, b);
    }
}

static void ed_scalarbase(gf p[4], const uint8_t *s)
{
    gf q[4];
    set25519(q[0], ed_X);
    set25519(q[1], ed_Y);
    set25519(q[2], ed_gf1);
    M25519(q[3], ed_X, ed_Y);
    ed_scalarmult(p, q, s);
}

static void ed_modL(uint8_t *r, int64_t x[64])
{
    int64_t carry;
    for (int i = 63; i >= 32; --i) {
        carry = 0;
        for (int j = i - 32, k = i - 12; j < k; ++j) {
            x[j] += carry - 16 * x[i] * (int64_t)ed_L[j - (i - 32)];
            carry = (x[j] + 128) >> 8;
            x[j] -= carry * 256;
        }
        x[i - 12] += carry;
        x[i] = 0;
    }
    carry = 0;
    for (int j = 0; j < 32; ++j) {
        x[j] += carry - (x[31] >> 4) * (int64_t)ed_L[j];
        carry = x[j] >> 8;
        x[j] &= 255;
    }
    for (int j = 0; j < 32; ++j) x[j] -= carry * (int64_t)ed_L[j];
    for (int i = 0; i < 32; ++i) {
        x[i + 1] += x[i] >> 8;
        r[i] = (uint8_t)(x[i] & 255);
    }
}

static void ed_reduce(uint8_t *r)
{
    int64_t x[64];
    for (int i = 0; i < 64; i++) x[i] = (uint64_t)r[i];
    for (int i = 0; i < 64; i++) r[i] = 0;
    ed_modL(r, x);
}

static void ed25519_sign(uint8_t sig[64], const uint8_t *msg, size_t mlen,
                         const uint8_t seed[32], const uint8_t pub[32])
{
    uint8_t d[64], r[64], hram[64];
    int64_t x[64];
    gf p[4];
    sha512_ctx_t ctx;

    sha512_init(&ctx);
    sha512_update(&ctx, seed, 32);
    sha512_final(&ctx, d);
    d[0] &= 248u;
    d[31] &= 127u;
    d[31] |= 64u;

    sha512_init(&ctx);
    sha512_update(&ctx, d + 32, 32);
    sha512_update(&ctx, msg, mlen);
    sha512_final(&ctx, r);
    ed_reduce(r);

    ed_scalarbase(p, r);
    ed_pack(sig, p);

    sha512_init(&ctx);
    sha512_update(&ctx, sig, 32);
    sha512_update(&ctx, pub, 32);
    sha512_update(&ctx, msg, mlen);
    sha512_final(&ctx, hram);
    ed_reduce(hram);

    for (int i = 0; i < 64; i++) x[i] = 0;
    for (int i = 0; i < 32; i++) x[i] = (uint64_t)r[i];
    for (int i = 0; i < 32; i++)
        for (int j = 0; j < 32; j++)
            x[i + j] += (int64_t)hram[i] * (int64_t)d[j];
    ed_modL(sig + 32, x);
}

/* --- AES-128-CTR --- */
static const uint8_t s_aes_sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

typedef struct {
    uint8_t rk[176];
    uint8_t iv[16];
} aes128_ctr_ctx_t;

static void aes128_init(aes128_ctr_ctx_t *ctx, const uint8_t key[16], const uint8_t iv[16])
{
    memcpy(ctx->rk, key, 16);
    memcpy(ctx->iv, iv, 16);
    uint8_t rcon = 1;
    for (int i = 4; i < 44; i++) {
        uint8_t t[4];
        memcpy(t, &ctx->rk[(i - 1) * 4], 4);
        if ((i & 3) == 0) {
            uint8_t u = t[0];
            t[0] = s_aes_sbox[t[1]] ^ rcon;
            t[1] = s_aes_sbox[t[2]];
            t[2] = s_aes_sbox[t[3]];
            t[3] = s_aes_sbox[u];
            rcon = (uint8_t)((rcon << 1) ^ ((rcon & 0x80) ? 0x1b : 0));
        }
        for (int j = 0; j < 4; j++) {
            ctx->rk[i * 4 + j] = ctx->rk[(i - 4) * 4 + j] ^ t[j];
        }
    }
}

static inline uint8_t xtime(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80u) ? 0x1bu : 0u)); }

static void aes128_encrypt_block(const uint8_t rk[176], const uint8_t in[16], uint8_t out[16])
{
    uint8_t s[16];
    for (int i = 0; i < 16; i++) s[i] = in[i] ^ rk[i];
    for (int r = 1; r <= 10; r++) {
        uint8_t t[16];
        for (int i = 0; i < 16; i++) t[i] = s_aes_sbox[s[i]];
        s[0]=t[0]; s[1]=t[5]; s[2]=t[10]; s[3]=t[15];
        s[4]=t[4]; s[5]=t[9]; s[6]=t[14]; s[7]=t[3];
        s[8]=t[8]; s[9]=t[13]; s[10]=t[2]; s[11]=t[7];
        s[12]=t[12]; s[13]=t[1]; s[14]=t[6]; s[15]=t[11];
        if (r < 10) {
            for (int c = 0; c < 4; c++) {
                uint8_t *col = &s[c * 4];
                uint8_t a0=col[0], a1=col[1], a2=col[2], a3=col[3];
                uint8_t tm = a0 ^ a1 ^ a2 ^ a3;
                col[0] ^= tm ^ xtime(a0 ^ a1);
                col[1] ^= tm ^ xtime(a1 ^ a2);
                col[2] ^= tm ^ xtime(a2 ^ a3);
                col[3] ^= tm ^ xtime(a3 ^ a0);
            }
        }
        for (int i = 0; i < 16; i++) s[i] ^= rk[r * 16 + i];
    }
    memcpy(out, s, 16);
}

/* RFC 4344 Section 4: AES-CTR increments 128-bit counter block as big-endian integer */
static void aes128_ctr_crypt(aes128_ctr_ctx_t *ctx, uint8_t *buf, size_t len)
{
    uint8_t ks[16];
    for (size_t off = 0; off < len; off += 16) {
        aes128_encrypt_block(ctx->rk, ctx->iv, ks);
        for (int i = 15; i >= 0; i--) {
            if (++ctx->iv[i] != 0) break;
        }
        size_t chunk = (len - off < 16) ? (len - off) : 16;
        for (size_t j = 0; j < chunk; j++) {
            buf[off + j] ^= ks[j];
        }
    }
}

/* PRNG seeded with microsecond timer + SHA-256 counter mode */
static uint32_t s_prng_counter = 1;
static void ssh_random_bytes(uint8_t *out, size_t len)
{
    size_t pos = 0;
    while (pos < len) {
        uint64_t us = hal_time_now_us();
        s_prng_counter += 0x9e3779b9u;
        sha256_ctx_t ctx;
        uint8_t digest[32];
        sha256_init(&ctx);
        sha256_update(&ctx, &us, sizeof(us));
        sha256_update(&ctx, &s_prng_counter, sizeof(s_prng_counter));
        sha256_final(&ctx, digest);
        size_t n = (len - pos < 32) ? (len - pos) : 32;
        memcpy(out + pos, digest, n);
        pos += n;
    }
}

/* =========================================================================
 * 2. SSH-2 Session Context & Buffer Helpers
 * ========================================================================= */

typedef struct {
    int              sock;
    uint32_t         seq_c2s;
    uint32_t         seq_s2c;
    bool             encrypted;
    aes128_ctr_ctx_t enc_c2s;
    aes128_ctr_ctx_t dec_s2c;
    uint8_t          mac_key_c2s[32];
    uint8_t          mac_key_s2c[32];
    uint8_t          session_id[32];
    char             v_s[128];
    uint8_t          i_c[512];
    size_t           i_c_len;
    uint8_t          i_s[2048];
    size_t           i_s_len;
    uint8_t          tx_pkt[SSH_MAX_PKT];
    uint8_t          rx_pkt[SSH_MAX_PKT];
    uint8_t          payload[SSH_MAX_PKT - 64];
    uint32_t         remote_chan;
    uint32_t         remote_win;
    uint32_t         local_win_consumed;
    int              exit_status;
} ssh_session_t;

static void put_u32_be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v);
}

static uint32_t get_u32_be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

static size_t buf_put_u32(uint8_t *buf, size_t pos, uint32_t v)
{
    put_u32_be(buf + pos, v);
    return pos + 4;
}

static size_t buf_put_u8(uint8_t *buf, size_t pos, uint8_t v)
{
    buf[pos] = v;
    return pos + 1;
}

static size_t buf_put_mem(uint8_t *buf, size_t pos, const void *data, size_t len)
{
    put_u32_be(buf + pos, (uint32_t)len);
    if (len > 0) memcpy(buf + pos + 4, data, len);
    return pos + 4 + len;
}

static size_t buf_put_str(uint8_t *buf, size_t pos, const char *s)
{
    size_t len = s ? strlen(s) : 0;
    return buf_put_mem(buf, pos, s, len);
}

static bool buf_get_u32(const uint8_t *buf, size_t len, size_t *pos, uint32_t *out)
{
    if (*pos + 4 > len) return false;
    *out = get_u32_be(buf + *pos);
    *pos += 4;
    return true;
}

static bool buf_get_str(const uint8_t *buf, size_t len, size_t *pos, const uint8_t **out_ptr, size_t *out_len)
{
    uint32_t slen = 0;
    if (!buf_get_u32(buf, len, pos, &slen)) return false;
    if (*pos + slen > len) return false;
    *out_ptr = buf + *pos;
    *out_len = slen;
    *pos += slen;
    return true;
}

static void sha256_update_ssh_str(sha256_ctx_t *ctx, const void *data, size_t len)
{
    uint8_t lbe[4];
    put_u32_be(lbe, (uint32_t)len);
    sha256_update(ctx, lbe, 4);
    if (len > 0) sha256_update(ctx, data, len);
}

/* Read exact `len` bytes from TCP socket `sock` within `timeout_ms`, returning -1 on error, 0 on timeout before first byte */
static int tcp_read_exact(int sock, uint8_t *buf, size_t len, uint32_t first_timeout_ms, uint32_t cont_timeout_ms)
{
    size_t got = 0;
    while (got < len) {
        uint32_t tmo = (got == 0) ? first_timeout_ms : cont_timeout_ms;
        int r = wifi_tcp_recv_sock(sock, buf + got, len - got, tmo);
        if (r < 0) return -1;
        if (r == 0) {
            if (got == 0) return 0;
            return -1;
        }
        got += (size_t)r;
    }
    return (int)got;
}

static bool ssh_send_packet(ssh_session_t *s, const uint8_t *payload, size_t payload_len)
{
    size_t block_sz = s->encrypted ? 16u : 8u;
    size_t unpadded = 5u + payload_len;
    size_t pad_len  = block_sz - (unpadded & (block_sz - 1u));
    if (pad_len < 4u) pad_len += block_sz;

    size_t pkt_len = 1u + payload_len + pad_len;
    size_t total_len = 4u + pkt_len;
    if (total_len + 32u > SSH_MAX_PKT) return false;

    put_u32_be(s->tx_pkt, (uint32_t)pkt_len);
    s->tx_pkt[4] = (uint8_t)pad_len;
    memcpy(s->tx_pkt + 5, payload, payload_len);
    ssh_random_bytes(s->tx_pkt + 5 + payload_len, pad_len);

    if (s->encrypted) {
        uint8_t mac[32];
        hmac_sha256_packet(s->mac_key_c2s, s->seq_c2s, s->tx_pkt, total_len, mac);
        aes128_ctr_crypt(&s->enc_c2s, s->tx_pkt, total_len);
        memcpy(s->tx_pkt + total_len, mac, 32);
        total_len += 32u;
    }

    s->seq_c2s++;
    return wifi_tcp_send_sock(s->sock, s->tx_pkt, total_len, 10000) == (int)total_len;
}

/*
 * Receive one SSH-2 packet.
 * Returns:
 *   > 0 : payload length (payload stored at *out_payload = s->rx_pkt + 5)
 *     0 : timeout waiting for packet
 *   < 0 : connection closed or protocol/MAC error
 */
static int ssh_recv_packet(ssh_session_t *s, uint8_t **out_payload, uint32_t timeout_ms)
{
    for (;;) {
        if (!s->encrypted) {
            int r = tcp_read_exact(s->sock, s->rx_pkt, 4, timeout_ms, 8000);
            if (r <= 0) return r;
            uint32_t pkt_len = get_u32_be(s->rx_pkt);
            if (pkt_len < 5u || 4u + pkt_len > SSH_MAX_PKT) {
                kprintf("ssh: bad unencrypted packet length %u\n", pkt_len);
                return -1;
            }
            if (tcp_read_exact(s->sock, s->rx_pkt + 4, pkt_len, 8000, 8000) != (int)pkt_len) return -1;
            uint8_t pad_len = s->rx_pkt[4];
            if ((uint32_t)pad_len + 1u >= pkt_len) return -1;
            size_t payload_len = pkt_len - 1u - pad_len;
            s->seq_s2c++;
            uint8_t msg_type = s->rx_pkt[5];
            if (msg_type == SSH_MSG_IGNORE || msg_type == SSH_MSG_DEBUG ||
                msg_type == SSH_MSG_UNIMPLEMENTED || msg_type == SSH_MSG_EXT_INFO) {
                continue;
            }
            if (msg_type == SSH_MSG_DISCONNECT) {
                size_t pos = 1;
                uint32_t reason = 0;
                const uint8_t *desc = NULL;
                size_t dlen = 0;
                buf_get_u32(s->rx_pkt + 5, payload_len, &pos, &reason);
                if (buf_get_str(s->rx_pkt + 5, payload_len, &pos, &desc, &dlen) && dlen > 0) {
                    kprintf("\nssh: remote disconnected (%u): ", reason);
                    kconsole_write((const char *)desc, dlen);
                    kprintf("\n");
                }
                return -1;
            }
            *out_payload = s->rx_pkt + 5;
            return (int)payload_len;
        } else {
            int r = tcp_read_exact(s->sock, s->rx_pkt, 16, timeout_ms, 8000);
            if (r <= 0) return r;
            aes128_ctr_crypt(&s->dec_s2c, s->rx_pkt, 16);
            uint32_t pkt_len = get_u32_be(s->rx_pkt);
            size_t total_len = 4u + pkt_len;
            if (pkt_len < 12u || total_len > SSH_MAX_PKT || (total_len & 15u) != 0) {
                kprintf("ssh: bad encrypted packet length %u\n", pkt_len);
                return -1;
            }
            if (total_len > 16u) {
                size_t rem = total_len - 16u;
                if (tcp_read_exact(s->sock, s->rx_pkt + 16, rem, 8000, 8000) != (int)rem) return -1;
                aes128_ctr_crypt(&s->dec_s2c, s->rx_pkt + 16, rem);
            }
            uint8_t rx_mac[32], exp_mac[32];
            if (tcp_read_exact(s->sock, rx_mac, 32, 8000, 8000) != 32) return -1;
            hmac_sha256_packet(s->mac_key_s2c, s->seq_s2c, s->rx_pkt, total_len, exp_mac);
            uint8_t diff = 0;
            for (int i = 0; i < 32; i++) diff |= (uint8_t)(rx_mac[i] ^ exp_mac[i]);
            if (diff != 0) {
                kprintf("ssh: MAC verification failed on seq %u\n", s->seq_s2c);
                return -1;
            }
            s->seq_s2c++;
            uint8_t pad_len = s->rx_pkt[4];
            if ((uint32_t)pad_len + 1u >= pkt_len) return -1;
            size_t payload_len = pkt_len - 1u - pad_len;
            uint8_t msg_type = s->rx_pkt[5];
            if (msg_type == SSH_MSG_IGNORE || msg_type == SSH_MSG_DEBUG ||
                msg_type == SSH_MSG_UNIMPLEMENTED || msg_type == SSH_MSG_EXT_INFO) {
                continue;
            }
            if (msg_type == SSH_MSG_DISCONNECT) {
                size_t pos = 1;
                uint32_t reason = 0;
                const uint8_t *desc = NULL;
                size_t dlen = 0;
                buf_get_u32(s->rx_pkt + 5, payload_len, &pos, &reason);
                if (buf_get_str(s->rx_pkt + 5, payload_len, &pos, &desc, &dlen) && dlen > 0) {
                    kprintf("\nssh: remote disconnected (%u): ", reason);
                    kconsole_write((const char *)desc, dlen);
                    kprintf("\n");
                }
                return -1;
            }
            *out_payload = s->rx_pkt + 5;
            return (int)payload_len;
        }
    }
}

/* =========================================================================
 * 3. Key Exchange (curve25519-sha256, RFC 8731) & Key Derivation
 * ========================================================================= */

static void ssh_derive_key(const uint8_t *k_mpint, size_t k_mpint_len,
                           const uint8_t h[32], const uint8_t session_id[32],
                           char letter, uint8_t out[32])
{
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, k_mpint, k_mpint_len);
    sha256_update(&ctx, h, 32);
    uint8_t ch = (uint8_t)letter;
    sha256_update(&ctx, &ch, 1);
    sha256_update(&ctx, session_id, 32);
    sha256_final(&ctx, out);
}

static bool ssh_handshake(ssh_session_t *s)
{
    /* 1. Send Client Identification String immediately (RFC 4253 Section 4.2) */
    const char *banner_wire = SSH_BANNER_CLIENT "\r\n";
    if (wifi_tcp_send_sock(s->sock, banner_wire, strlen(banner_wire), 8000) != (int)strlen(banner_wire)) {
        kprintf("ssh: failed to send client banner\n");
        return false;
    }

    /* 2. Read Server Identification String (RFC 4253 Section 4.2) */
    for (;;) {
        size_t len = 0;
        while (len < sizeof(s->v_s) - 1) {
            uint8_t c = 0;
            int r = tcp_read_exact(s->sock, &c, 1, 8000, 8000);
            if (r <= 0) {
                kprintf("ssh: timeout waiting for server banner\n");
                return false;
            }
            if (c == '\n') break;
            if (c != '\r') s->v_s[len++] = (char)c;
        }
        s->v_s[len] = 0;
        if (len >= 4 && memcmp(s->v_s, "SSH-", 4) == 0) break;
    }

    /* 3. Build and send client SSH_MSG_KEXINIT (20) */
    size_t p = 0;
    p = buf_put_u8(s->i_c, p, SSH_MSG_KEXINIT);
    ssh_random_bytes(s->i_c + p, 16);
    p += 16;
    p = buf_put_str(s->i_c, p, "curve25519-sha256,curve25519-sha256@libssh.org");
    p = buf_put_str(s->i_c, p, "ssh-ed25519,ecdsa-sha2-nistp256,rsa-sha2-256,rsa-sha2-512,ssh-rsa");
    p = buf_put_str(s->i_c, p, "aes128-ctr");
    p = buf_put_str(s->i_c, p, "aes128-ctr");
    p = buf_put_str(s->i_c, p, "hmac-sha2-256");
    p = buf_put_str(s->i_c, p, "hmac-sha2-256");
    p = buf_put_str(s->i_c, p, "none");
    p = buf_put_str(s->i_c, p, "none");
    p = buf_put_str(s->i_c, p, "");
    p = buf_put_str(s->i_c, p, "");
    p = buf_put_u8(s->i_c, p, 0);     /* first_kex_packet_follows = FALSE */
    p = buf_put_u32(s->i_c, p, 0);    /* reserved = 0 */
    s->i_c_len = p;

    if (!ssh_send_packet(s, s->i_c, s->i_c_len)) return false;

    /* 4. Receive server SSH_MSG_KEXINIT (20) */
    uint8_t *rx = NULL;
    int rx_len = ssh_recv_packet(s, &rx, 10000);
    if (rx_len <= 0 || rx[0] != SSH_MSG_KEXINIT || (size_t)rx_len > sizeof(s->i_s)) {
        kprintf("ssh: expected KEXINIT from server\n");
        return false;
    }
    memcpy(s->i_s, rx, (size_t)rx_len);
    s->i_s_len = (size_t)rx_len;

    /* 5. Generate ephemeral X25519 keypair and send SSH_MSG_KEX_ECDH_INIT (30) */
    uint8_t priv_c[32], q_c[32];
    static const uint8_t basepoint[32] = { 9 };
    ssh_random_bytes(priv_c, 32);
    priv_c[0]  &= 248u;
    priv_c[31]  = (uint8_t)((priv_c[31] & 127u) | 64u);
    x25519_scalarmult(q_c, priv_c, basepoint);

    p = 0;
    p = buf_put_u8(s->payload, p, SSH_MSG_KEX_ECDH_INIT);
    p = buf_put_mem(s->payload, p, q_c, 32);
    if (!ssh_send_packet(s, s->payload, p)) return false;

    /* 6. Receive SSH_MSG_KEX_ECDH_REPLY (31) */
    rx_len = ssh_recv_packet(s, &rx, 10000);
    if (rx_len <= 0 || rx[0] != SSH_MSG_KEX_ECDH_REPLY) {
        kprintf("ssh: expected KEX_ECDH_REPLY (got %d)\n", rx_len > 0 ? rx[0] : rx_len);
        return false;
    }
    size_t pos = 1;
    const uint8_t *k_s = NULL, *q_s = NULL, *sig_h = NULL;
    size_t k_s_len = 0, q_s_len = 0, sig_h_len = 0;
    if (!buf_get_str(rx, (size_t)rx_len, &pos, &k_s, &k_s_len) ||
        !buf_get_str(rx, (size_t)rx_len, &pos, &q_s, &q_s_len) ||
        !buf_get_str(rx, (size_t)rx_len, &pos, &sig_h, &sig_h_len) ||
        q_s_len != 32) {
        kprintf("ssh: malformed KEX_ECDH_REPLY\n");
        return false;
    }

    /* Display Server Host Key Type & SHA-256 Fingerprint */
    uint8_t hk_fp[32];
    sha256_ctx_t hctx;
    sha256_init(&hctx);
    sha256_update(&hctx, k_s, k_s_len);
    sha256_final(&hctx, hk_fp);

    size_t hk_pos = 0;
    const uint8_t *hk_type = NULL;
    size_t hk_type_len = 0;
    char hk_type_str[32] = "hostkey";
    if (buf_get_str(k_s, k_s_len, &hk_pos, &hk_type, &hk_type_len) && hk_type_len < sizeof(hk_type_str)) {
        memcpy(hk_type_str, hk_type, hk_type_len);
        hk_type_str[hk_type_len] = 0;
    }
    kprintf("ssh: server %s (%s) SHA256:", s->v_s, hk_type_str);
    for (int i = 0; i < 8; i++) kprintf("%02x", hk_fp[i]);
    kprintf("...\n");

    /* 7. Compute shared secret K = X25519(priv_c, Q_S) and encode as SSH mpint */
    uint8_t k_raw[32];
    x25519_scalarmult(k_raw, priv_c, q_s);
    uint8_t nz = 0;
    for (int i = 0; i < 32; i++) nz |= k_raw[i];
    if (nz == 0) {
        kprintf("ssh: invalid X25519 public key\n");
        return false;
    }

    int first_nz = 0;
    while (first_nz < 32 && k_raw[first_nz] == 0) first_nz++;
    uint8_t k_mpint[37];
    size_t k_mpint_len = 0;
    if (first_nz == 32) {
        put_u32_be(k_mpint, 0);
        k_mpint_len = 4;
    } else {
        size_t rem = (size_t)(32 - first_nz);
        bool need_zero = (k_raw[first_nz] & 0x80u) != 0;
        put_u32_be(k_mpint, (uint32_t)(rem + (need_zero ? 1u : 0u)));
        size_t off = 4;
        if (need_zero) k_mpint[off++] = 0x00u;
        memcpy(k_mpint + off, k_raw + first_nz, rem);
        k_mpint_len = off + rem;
    }

    /* 8. Compute Exchange Hash H = SHA256(V_C || V_S || I_C || I_S || K_S || Q_C || Q_S || K) */
    uint8_t h[32];
    sha256_init(&hctx);
    sha256_update_ssh_str(&hctx, SSH_BANNER_CLIENT, strlen(SSH_BANNER_CLIENT));
    sha256_update_ssh_str(&hctx, s->v_s, strlen(s->v_s));
    sha256_update_ssh_str(&hctx, s->i_c, s->i_c_len);
    sha256_update_ssh_str(&hctx, s->i_s, s->i_s_len);
    sha256_update_ssh_str(&hctx, k_s, k_s_len);
    sha256_update_ssh_str(&hctx, q_c, 32);
    sha256_update_ssh_str(&hctx, q_s, 32);
    sha256_update(&hctx, k_mpint, k_mpint_len);
    sha256_final(&hctx, h);
    memcpy(s->session_id, h, 32);

    /* 9. Exchange SSH_MSG_NEWKEYS (21) */
    s->payload[0] = SSH_MSG_NEWKEYS;
    if (!ssh_send_packet(s, s->payload, 1)) return false;

    rx_len = ssh_recv_packet(s, &rx, 10000);
    if (rx_len <= 0 || rx[0] != SSH_MSG_NEWKEYS) {
        kprintf("ssh: expected NEWKEYS from server\n");
        return false;
    }

    /* 10. Derive directional IVs, AES-128 keys, and HMAC-SHA-256 keys */
    uint8_t iv_c2s[32], iv_s2c[32], key_c2s[32], key_s2c[32];
    ssh_derive_key(k_mpint, k_mpint_len, h, s->session_id, 'A', iv_c2s);
    ssh_derive_key(k_mpint, k_mpint_len, h, s->session_id, 'B', iv_s2c);
    ssh_derive_key(k_mpint, k_mpint_len, h, s->session_id, 'C', key_c2s);
    ssh_derive_key(k_mpint, k_mpint_len, h, s->session_id, 'D', key_s2c);
    ssh_derive_key(k_mpint, k_mpint_len, h, s->session_id, 'E', s->mac_key_c2s);
    ssh_derive_key(k_mpint, k_mpint_len, h, s->session_id, 'F', s->mac_key_s2c);

    aes128_init(&s->enc_c2s, key_c2s, iv_c2s);
    aes128_init(&s->dec_s2c, key_s2c, iv_s2c);
    s->encrypted = true;
    return true;
}

/* =========================================================================
 * 4. User Authentication (password & keyboard-interactive)
 * ========================================================================= */

static bool read_password_console(const char *prompt, char *out, size_t max_len)
{
    kprintf("%s", prompt);
    size_t len = 0;
    int prev = 0;
    for (;;) {
        int c = kconsole_getc();
        if (c < 0) {
            task_sleep(10);
            continue;
        }
        if (c == '\n' && prev == '\r') {
            prev = c;
            continue;
        }
        prev = c;
        if (c == '\r' || c == '\n') {
            kprintf("\n");
            out[len] = 0;
            return true;
        }
        if (c == 0x03) { /* Ctrl-C */
            kprintf("^C\n");
            out[0] = 0;
            return false;
        }
        if (c == 0x08 || c == 0x7F) {
            if (len > 0) len--;
            continue;
        }
        if (c >= 0x20 && c < 0x7F && len + 1 < max_len) {
            out[len++] = (char)c;
        }
    }
}

/* =========================================================================
 * 4b. Persistent Ed25519 Private Key Storage (SPI Flash 0x3F0000 + /home/id_ed25519)
 * ========================================================================= */

#define SSH_FLASH_MAGIC0 0x4D35534Bu /* "M5SK" */
#define SSH_FLASH_MAGIC1 0x45443235u /* "ED25" */

typedef struct {
    uint32_t magic0;
    uint32_t magic1;
    uint8_t  seed[32];
    uint8_t  pub[32];
    uint32_t crc;
    uint32_t reserved[13];
} ssh_flash_key_t;

static void ed25519_seed_to_pub(const uint8_t seed[32], uint8_t pub[32])
{
    uint8_t d[64];
    gf p[4];
    sha512_ctx_t ctx;
    sha512_init(&ctx);
    sha512_update(&ctx, seed, 32);
    sha512_final(&ctx, d);
    d[0]  &= 248u;
    d[31] &= 127u;
    d[31] |= 64u;
    ed_scalarbase(p, d);
    ed_pack(pub, p);
    memset(d, 0, sizeof(d));
}

#if M5OS_BOARD_TAB5
extern esp_rom_spiflash_result_t esp_rom_spiflash_unlock(void);

static bool IRAM_ATTR ssh_flash_read_record(ssh_flash_key_t *out)
{
    uint32_t flags = arch_irq_disable();
    esp_rom_spiflash_result_t r = esp_rom_spiflash_read(SSH_FLASH_KEY_ADDR, (uint32_t *)out, sizeof(*out));
    arch_irq_restore(flags);
    return r == ESP_ROM_SPIFLASH_RESULT_OK;
}

static bool IRAM_ATTR ssh_flash_write_record(const ssh_flash_key_t *rec)
{
    uint32_t flags = arch_irq_disable();
    esp_rom_spiflash_unlock();
    esp_rom_spiflash_result_t r = esp_rom_spiflash_erase_sector(SSH_FLASH_KEY_ADDR / 4096u);
    if (r == ESP_ROM_SPIFLASH_RESULT_OK) {
        r = esp_rom_spiflash_write(SSH_FLASH_KEY_ADDR, (const uint32_t *)rec, sizeof(*rec));
    }
    arch_irq_restore(flags);
    return r == ESP_ROM_SPIFLASH_RESULT_OK;
}

static bool IRAM_ATTR ssh_flash_erase_record(void)
{
    uint32_t flags = arch_irq_disable();
    esp_rom_spiflash_unlock();
    esp_rom_spiflash_result_t r = esp_rom_spiflash_erase_sector(SSH_FLASH_KEY_ADDR / 4096u);
    arch_irq_restore(flags);
    return r == ESP_ROM_SPIFLASH_RESULT_OK;
}
#else
static bool ssh_flash_read_record(ssh_flash_key_t *out) { (void)out; return false; }
static bool ssh_flash_write_record(const ssh_flash_key_t *rec) { (void)rec; return false; }
static bool ssh_flash_erase_record(void) { return false; }
#endif

static void ssh_format_pubkey_openssh(const uint8_t pub[32], char *out, size_t out_cap)
{
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint8_t blob[51];
    size_t n = 0;
    n = buf_put_str(blob, n, "ssh-ed25519");
    n = buf_put_mem(blob, n, pub, 32);

    char enc[80];
    size_t ei = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = ((uint32_t)blob[i] << 16) |
                     ((i + 1 < n ? (uint32_t)blob[i + 1] : 0u) << 8) |
                     (i + 2 < n ? (uint32_t)blob[i + 2] : 0u);
        enc[ei++] = b64[(v >> 18) & 0x3Fu];
        enc[ei++] = b64[(v >> 12) & 0x3Fu];
        enc[ei++] = (i + 1 < n) ? b64[(v >> 6) & 0x3Fu] : '=';
        enc[ei++] = (i + 2 < n) ? b64[v & 0x3Fu] : '=';
    }
    enc[ei] = 0;
    ksnprintf(out, out_cap, "ssh-ed25519 %s meos@tab5", enc);
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/*
 * Parse an Ed25519 32-byte seed from:
 *  - 64 hex characters, OR
 *  - OpenSSH private key ("-----BEGIN OPENSSH PRIVATE KEY-----" ... Base64), OR
 *  - Raw 32-byte or 64-byte binary buffer
 */
static bool ssh_parse_ed25519_seed(const uint8_t *in, size_t in_len, uint8_t out_seed[32], uint8_t out_pub[32])
{
    if (!in || in_len < 32) return false;

    /* 1. Try 64-char hex seed (ignoring leading/trailing whitespace) */
    size_t s = 0, e = in_len;
    while (s < e && (in[s] == ' ' || in[s] == '\t' || in[s] == '\r' || in[s] == '\n')) s++;
    while (e > s && (in[e - 1] == ' ' || in[e - 1] == '\t' || in[e - 1] == '\r' || in[e - 1] == '\n')) e--;
    if (e - s == 64 || e - s == 128) {
        bool all_hex = true;
        for (size_t i = 0; i < 64; i++) {
            if (hex_nibble((char)in[s + i]) < 0) { all_hex = false; break; }
        }
        if (all_hex) {
            for (size_t i = 0; i < 32; i++) {
                out_seed[i] = (uint8_t)((hex_nibble((char)in[s + i * 2]) << 4) |
                                         hex_nibble((char)in[s + i * 2 + 1]));
            }
            ed25519_seed_to_pub(out_seed, out_pub);
            return true;
        }
    }

    /* 2. Try OpenSSH PEM / Base64 ("openssh-key-v1") */
    const char *text = (const char *)in;
    const char *b64_start = text;
    const char *b64_end   = text + in_len;
    const char *hdr = strstr(text, "-----BEGIN OPENSSH PRIVATE KEY-----");
    if (hdr) {
        b64_start = hdr + 35;
        const char *ftr = strstr(b64_start, "-----END OPENSSH PRIVATE KEY-----");
        if (ftr) b64_end = ftr;
    }

    uint8_t raw[600];
    size_t rlen = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (const char *p = b64_start; p < b64_end && rlen < sizeof(raw); p++) {
        int v = b64_val(*p);
        if (v < 0) continue;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            raw[rlen++] = (uint8_t)((acc >> bits) & 0xFFu);
        }
    }

    if (rlen >= 120 && memcmp(raw, "openssh-key-v1\0", 15) == 0) {
        /* Locate 0x00000040 (64-byte Ed25519 sk = seed[32] || pub[32]) preceded by 0x00000020 pub[32] */
        for (size_t i = 15; i + 4 + 32 + 4 + 64 <= rlen; i++) {
            if (get_u32_be(raw + i) == 32u && get_u32_be(raw + i + 36) == 64u) {
                const uint8_t *cand_pub  = raw + i + 4;
                const uint8_t *cand_seed = raw + i + 40;
                const uint8_t *cand_pub2 = raw + i + 72;
                if (memcmp(cand_pub, cand_pub2, 32) == 0) {
                    uint8_t derived[32];
                    ed25519_seed_to_pub(cand_seed, derived);
                    if (memcmp(derived, cand_pub, 32) == 0) {
                        memcpy(out_seed, cand_seed, 32);
                        memcpy(out_pub, derived, 32);
                        memset(raw, 0, sizeof(raw));
                        return true;
                    }
                }
            }
        }
    }
    memset(raw, 0, sizeof(raw));

    /* 3. Try raw 64-byte (seed[32] || pub[32]) or 32-byte binary */
    if (in_len == 64) {
        uint8_t derived[32];
        ed25519_seed_to_pub(in, derived);
        if (memcmp(derived, in + 32, 32) == 0) {
            memcpy(out_seed, in, 32);
            memcpy(out_pub, derived, 32);
            return true;
        }
    } else if (in_len == 32) {
        memcpy(out_seed, in, 32);
        ed25519_seed_to_pub(out_seed, out_pub);
        return true;
    }

    return false;
}

static void ssh_write_vfs_key_files(const uint8_t seed[32], const uint8_t pub[32])
{
    int wfd = vfs_open(SSH_KEY_PATH, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
    if (wfd >= 0) {
        uint8_t buf[64];
        memcpy(buf, seed, 32);
        memcpy(buf + 32, pub, 32);
        vfs_write(wfd, buf, 64);
        vfs_close(wfd);
        memset(buf, 0, sizeof(buf));
    }
    int pfd = vfs_open(SSH_PUBKEY_PATH, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
    if (pfd >= 0) {
        char pub_line[128];
        ssh_format_pubkey_openssh(pub, pub_line, sizeof(pub_line));
        size_t l = strlen(pub_line);
        pub_line[l++] = '\n';
        vfs_write(pfd, pub_line, l);
        vfs_close(pfd);
    }
}

static bool ssh_save_ed25519_key(const uint8_t seed[32], uint8_t out_pub[32])
{
    uint8_t pub[32];
    ed25519_seed_to_pub(seed, pub);
    if (out_pub) memcpy(out_pub, pub, 32);

    ssh_write_vfs_key_files(seed, pub);

    ssh_flash_key_t rec __attribute__((aligned(4)));
    memset(&rec, 0, sizeof(rec));
    rec.magic0 = SSH_FLASH_MAGIC0;
    rec.magic1 = SSH_FLASH_MAGIC1;
    memcpy(rec.seed, seed, 32);
    memcpy(rec.pub,  pub,  32);
    rec.crc = crc32(rec.seed, 64);
    bool flash_ok = ssh_flash_write_record(&rec);
    memset(&rec, 0, sizeof(rec));
    return flash_ok;
}

static bool ssh_load_ed25519_key(uint8_t seed[32], uint8_t pub[32], bool *from_flash_out)
{
    if (from_flash_out) *from_flash_out = false;

    /* 1. Check VFS (/home/id_ed25519) first */
    int fd = vfs_open(SSH_KEY_PATH, VFS_O_READ);
    if (fd >= 0) {
        static uint8_t fbuf[768];
        int n = vfs_read(fd, fbuf, sizeof(fbuf) - 1);
        vfs_close(fd);
        if (n >= 32) {
            fbuf[n] = 0;
            if (ssh_parse_ed25519_seed(fbuf, (size_t)n, seed, pub)) {
                memset(fbuf, 0, sizeof(fbuf));
                return true;
            }
        }
        memset(fbuf, 0, sizeof(fbuf));
    }

    /* 2. Check non-volatile SPI Flash keystore at 0x3F0000 */
    ssh_flash_key_t rec __attribute__((aligned(4)));
    memset(&rec, 0, sizeof(rec));
    if (ssh_flash_read_record(&rec) &&
        rec.magic0 == SSH_FLASH_MAGIC0 &&
        rec.magic1 == SSH_FLASH_MAGIC1 &&
        rec.crc == crc32(rec.seed, 64)) {
        memcpy(seed, rec.seed, 32);
        memcpy(pub,  rec.pub,  32);
        memset(&rec, 0, sizeof(rec));
        if (from_flash_out) *from_flash_out = true;
        /* Mirror into VFS /home/id_ed25519 & /home/id_ed25519.pub for this boot */
        ssh_write_vfs_key_files(seed, pub);
        return true;
    }
    memset(&rec, 0, sizeof(rec));
    return false;
}

static void ssh_clear_ed25519_key(void)
{
    vfs_unlink(SSH_KEY_PATH);
    vfs_unlink(SSH_PUBKEY_PATH);
    ssh_flash_erase_record();
}

/*
 * RFC 4252 Section 7 & RFC 8709: SSH-2 "publickey" authentication with "ssh-ed25519"
 */
static bool ssh_try_publickey_ed25519(ssh_session_t *s, const char *user)
{
    uint8_t seed[32], pub[32];
    if (!ssh_load_ed25519_key(seed, pub, NULL)) {
        return false;
    }

    /* 1. Build ssh-ed25519 public key blob (51 bytes): string("ssh-ed25519") || string(pub[32]) */
    uint8_t pk_blob[64];
    size_t pk_len = 0;
    pk_len = buf_put_str(pk_blob, pk_len, "ssh-ed25519");
    pk_len = buf_put_mem(pk_blob, pk_len, pub, 32);

    /*
     * 2. Build data to sign (RFC 4252 Section 7):
     *    string    session identifier (32 bytes -> 36 bytes encoded)
     *    byte      SSH_MSG_USERAUTH_REQUEST (50)
     *    string    user name
     *    string    "ssh-connection"
     *    string    "publickey"
     *    boolean   TRUE (1)
     *    string    "ssh-ed25519"
     *    string    public key blob
     */
    uint8_t sign_buf[384];
    size_t sp = 0;
    sp = buf_put_mem(sign_buf, sp, s->session_id, 32);
    size_t req_offset = sp; /* Offset 36: start of SSH_MSG_USERAUTH_REQUEST fields */
    sp = buf_put_u8(sign_buf, sp, SSH_MSG_USERAUTH_REQUEST);
    sp = buf_put_str(sign_buf, sp, user);
    sp = buf_put_str(sign_buf, sp, "ssh-connection");
    sp = buf_put_str(sign_buf, sp, "publickey");
    sp = buf_put_u8(sign_buf, sp, 1);
    sp = buf_put_str(sign_buf, sp, "ssh-ed25519");
    sp = buf_put_mem(sign_buf, sp, pk_blob, pk_len);

    uint8_t sig64[64];
    ed25519_sign(sig64, sign_buf, sp, seed, pub);
    memset(seed, 0, sizeof(seed));

    /* 3. Build ssh-ed25519 signature blob (83 bytes): string("ssh-ed25519") || string(sig64[64]) */
    uint8_t sig_blob[96];
    size_t sig_len = 0;
    sig_len = buf_put_str(sig_blob, sig_len, "ssh-ed25519");
    sig_len = buf_put_mem(sig_blob, sig_len, sig64, 64);

    /* 4. Send SSH_MSG_USERAUTH_REQUEST with signature */
    size_t p = sp - req_offset;
    memcpy(s->payload, sign_buf + req_offset, p);
    p = buf_put_mem(s->payload, p, sig_blob, sig_len);
    if (!ssh_send_packet(s, s->payload, p)) return false;

    uint8_t *rx = NULL;
    for (;;) {
        int rx_len = ssh_recv_packet(s, &rx, 10000);
        if (rx_len <= 0) return false;
        if (rx[0] == SSH_MSG_USERAUTH_BANNER) {
            size_t pos = 1;
            const uint8_t *bmsg = NULL;
            size_t blen = 0;
            if (buf_get_str(rx, (size_t)rx_len, &pos, &bmsg, &blen) && blen > 0) {
                kconsole_write((const char *)bmsg, blen);
            }
            continue;
        }
        if (rx[0] == SSH_MSG_USERAUTH_SUCCESS) {
            return true;
        }
        if (rx[0] == SSH_MSG_USERAUTH_FAILURE) {
            return false;
        }
        return false;
    }
}

static bool ssh_authenticate(ssh_session_t *s, const char *user, const char *host,
                             char *password, size_t pass_cap, bool gui_mode)
{
    /* Request ssh-userauth service */
    size_t p = 0;
    p = buf_put_u8(s->payload, p, SSH_MSG_SERVICE_REQUEST);
    p = buf_put_str(s->payload, p, "ssh-userauth");
    if (!ssh_send_packet(s, s->payload, p)) return false;

    uint8_t *rx = NULL;
    int rx_len = ssh_recv_packet(s, &rx, 10000);
    if (rx_len <= 0 || rx[0] != SSH_MSG_SERVICE_ACCEPT) {
        kprintf("ssh: ssh-userauth service rejected\n");
        return false;
    }

    /* Probe "none" auth first */
    p = 0;
    p = buf_put_u8(s->payload, p, SSH_MSG_USERAUTH_REQUEST);
    p = buf_put_str(s->payload, p, user);
    p = buf_put_str(s->payload, p, "ssh-connection");
    p = buf_put_str(s->payload, p, "none");
    if (!ssh_send_packet(s, s->payload, p)) return false;

    for (;;) {
        rx_len = ssh_recv_packet(s, &rx, 10000);
        if (rx_len <= 0) return false;
        if (rx[0] == SSH_MSG_USERAUTH_BANNER) {
            size_t pos = 1;
            const uint8_t *bmsg = NULL;
            size_t blen = 0;
            if (buf_get_str(rx, (size_t)rx_len, &pos, &bmsg, &blen) && blen > 0) {
                kconsole_write((const char *)bmsg, blen);
            }
            continue;
        }
        if (rx[0] == SSH_MSG_USERAUTH_SUCCESS) return true;
        if (rx[0] == SSH_MSG_USERAUTH_FAILURE) break;
        return false;
    }

    /* Try "publickey" (ssh-ed25519) authentication before prompting for password */
    if (ssh_try_publickey_ed25519(s, user)) {
        return true;
    }

    /* Prompt for password if none was provided yet */
    if (!password[0]) {
        if (gui_mode) {
            char prompt_title[80];
            ksnprintf(prompt_title, sizeof(prompt_title), "Password for %s@%s", user, host);
            if (!dq_input(prompt_title, password, pass_cap)) return false;
        } else {
            char prompt[96];
            ksnprintf(prompt, sizeof(prompt), "%s@%s's password: ", user, host);
            if (!read_password_console(prompt, password, pass_cap)) return false;
        }
    }

    /* Try "password" method */
    p = 0;
    p = buf_put_u8(s->payload, p, SSH_MSG_USERAUTH_REQUEST);
    p = buf_put_str(s->payload, p, user);
    p = buf_put_str(s->payload, p, "ssh-connection");
    p = buf_put_str(s->payload, p, "password");
    p = buf_put_u8(s->payload, p, 0);
    p = buf_put_str(s->payload, p, password);
    if (!ssh_send_packet(s, s->payload, p)) return false;

    for (;;) {
        rx_len = ssh_recv_packet(s, &rx, 10000);
        if (rx_len <= 0) return false;
        if (rx[0] == SSH_MSG_USERAUTH_BANNER) {
            size_t pos = 1;
            const uint8_t *bmsg = NULL;
            size_t blen = 0;
            if (buf_get_str(rx, (size_t)rx_len, &pos, &bmsg, &blen) && blen > 0) {
                kconsole_write((const char *)bmsg, blen);
            }
            continue;
        }
        if (rx[0] == SSH_MSG_USERAUTH_SUCCESS) return true;
        if (rx[0] == SSH_MSG_USERAUTH_FAILURE) break;
        return false;
    }

    /* Fallback: try "keyboard-interactive" (PAM on macOS/Linux) */
    p = 0;
    p = buf_put_u8(s->payload, p, SSH_MSG_USERAUTH_REQUEST);
    p = buf_put_str(s->payload, p, user);
    p = buf_put_str(s->payload, p, "ssh-connection");
    p = buf_put_str(s->payload, p, "keyboard-interactive");
    p = buf_put_str(s->payload, p, "");
    p = buf_put_str(s->payload, p, "");
    if (!ssh_send_packet(s, s->payload, p)) return false;

    for (;;) {
        rx_len = ssh_recv_packet(s, &rx, 10000);
        if (rx_len <= 0) return false;
        if (rx[0] == SSH_MSG_USERAUTH_BANNER) {
            size_t pos = 1;
            const uint8_t *bmsg = NULL;
            size_t blen = 0;
            if (buf_get_str(rx, (size_t)rx_len, &pos, &bmsg, &blen) && blen > 0) {
                kconsole_write((const char *)bmsg, blen);
            }
            continue;
        }
        if (rx[0] == SSH_MSG_USERAUTH_SUCCESS) return true;
        if (rx[0] == SSH_MSG_USERAUTH_INFO_REQUEST) {
            size_t pos = 1;
            const uint8_t *name = NULL, *inst = NULL, *lang = NULL;
            size_t nlen = 0, ilen = 0, llen = 0;
            uint32_t num_prompts = 0;
            if (!buf_get_str(rx, (size_t)rx_len, &pos, &name, &nlen) ||
                !buf_get_str(rx, (size_t)rx_len, &pos, &inst, &ilen) ||
                !buf_get_str(rx, (size_t)rx_len, &pos, &lang, &llen) ||
                !buf_get_u32(rx, (size_t)rx_len, &pos, &num_prompts)) {
                return false;
            }
            p = 0;
            p = buf_put_u8(s->payload, p, SSH_MSG_USERAUTH_INFO_RESPONSE);
            p = buf_put_u32(s->payload, p, num_prompts);
            for (uint32_t i = 0; i < num_prompts; i++) {
                p = buf_put_str(s->payload, p, password);
            }
            if (!ssh_send_packet(s, s->payload, p)) return false;
            continue;
        }
        if (rx[0] == SSH_MSG_USERAUTH_FAILURE) {
            kprintf("ssh: Permission denied (publickey,password,keyboard-interactive)\n");
            return false;
        }
        return false;
    }
}

/* =========================================================================
 * 5. Saved SSH Profiles & Password Manager (/home/ssh_hosts.cfg)
 * ========================================================================= */

#define SSH_MAX_PROFILES  16

typedef struct {
    char     user[32];
    char     host[64];
    uint16_t port;
    char     pass[64];
} ssh_profile_t;

static ssh_profile_t s_profiles[SSH_MAX_PROFILES];
static int           s_profile_count = 0;
static bool          s_profiles_loaded = false;

static void parse_user_host_port(const char *spec, char *user, size_t ucap,
                                 char *host, size_t hcap, uint16_t *port)
{
    const char *at = strchr(spec, '@');
    const char *hstart = spec;
    if (at) {
        size_t ulen = (size_t)(at - spec);
        if (ulen >= ucap) ulen = ucap - 1;
        memcpy(user, spec, ulen);
        user[ulen] = 0;
        hstart = at + 1;
    } else if (!user[0]) {
        strncpy(user, "root", ucap - 1);
        user[ucap - 1] = 0;
    }

    const char *colon = strchr(hstart, ':');
    if (colon) {
        size_t hlen = (size_t)(colon - hstart);
        if (hlen >= hcap) hlen = hcap - 1;
        memcpy(host, hstart, hlen);
        host[hlen] = 0;
        uint32_t p = 0;
        for (const char *s = colon + 1; *s >= '0' && *s <= '9'; s++) {
            p = p * 10u + (uint32_t)(*s - '0');
        }
        if (p > 0 && p <= 65535u) *port = (uint16_t)p;
    } else {
        strncpy(host, hstart, hcap - 1);
        host[hcap - 1] = 0;
    }
}

static void ssh_profiles_save(void)
{
    int fd = vfs_open(SSH_HOSTS_CFG_PATH, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
    if (fd < 0) return;
    for (int i = 0; i < s_profile_count; i++) {
        char line[196];
        int n = ksnprintf(line, sizeof(line), "%s\t%s\t%u\t%s\n",
                          s_profiles[i].user,
                          s_profiles[i].host,
                          (unsigned)s_profiles[i].port,
                          s_profiles[i].pass);
        if (n > 0) {
            vfs_write(fd, line, (size_t)n);
        }
    }
    vfs_close(fd);
}

static void ssh_profiles_load(void)
{
    s_profile_count = 0;
    s_profiles_loaded = true;

    int fd = vfs_open(SSH_HOSTS_CFG_PATH, VFS_O_READ);
    if (fd >= 0) {
        static char buf[2048];
        int n = vfs_read(fd, buf, sizeof(buf) - 1);
        vfs_close(fd);
        if (n > 0) {
            buf[n] = 0;
            char *p = buf;
            while (*p && s_profile_count < SSH_MAX_PROFILES) {
                char *eol = p;
                while (*eol && *eol != '\n' && *eol != '\r') eol++;
                char saved_ch = *eol;
                *eol = 0;

                if (*p) {
                    char *f1 = p;
                    char *f2 = strchr(f1, '\t');
                    if (f2) {
                        *f2++ = 0;
                        char *f3 = strchr(f2, '\t');
                        if (f3) {
                            *f3++ = 0;
                            char *f4 = strchr(f3, '\t');
                            if (f4) *f4++ = 0;

                            ssh_profile_t *pr = &s_profiles[s_profile_count];
                            memset(pr, 0, sizeof(*pr));
                            strncpy(pr->user, f1, sizeof(pr->user) - 1);
                            strncpy(pr->host, f2, sizeof(pr->host) - 1);
                            uint32_t port = 0;
                            for (const char *s = f3; *s >= '0' && *s <= '9'; s++) {
                                port = port * 10u + (uint32_t)(*s - '0');
                            }
                            pr->port = (port > 0 && port <= 65535u) ? (uint16_t)port : 22u;
                            if (f4) strncpy(pr->pass, f4, sizeof(pr->pass) - 1);
                            if (pr->user[0] && pr->host[0]) {
                                s_profile_count++;
                            }
                        }
                    }
                }

                if (!saved_ch) break;
                p = eol + 1;
                while (*p == '\r' || *p == '\n') p++;
            }
        }
    }

    /* Migrate legacy /home/ssh.cfg if present and not already in list */
    int lfd = vfs_open(SSH_CFG_PATH, VFS_O_READ);
    if (lfd >= 0) {
        char lbuf[96];
        int ln = vfs_read(lfd, lbuf, sizeof(lbuf) - 1);
        vfs_close(lfd);
        if (ln > 0) {
            lbuf[ln] = 0;
            for (int i = 0; i < ln; i++) {
                if (lbuf[i] == '\r' || lbuf[i] == '\n') { lbuf[i] = 0; break; }
            }
            if (lbuf[0] && s_profile_count < SSH_MAX_PROFILES) {
                char u[32] = "";
                char h[64] = "";
                uint16_t pt = 22;
                parse_user_host_port(lbuf, u, sizeof(u), h, sizeof(h), &pt);
                if (u[0] && h[0]) {
                    bool exists = false;
                    for (int i = 0; i < s_profile_count; i++) {
                        if (strcmp(s_profiles[i].user, u) == 0 &&
                            strcmp(s_profiles[i].host, h) == 0 &&
                            s_profiles[i].port == pt) {
                            exists = true;
                            break;
                        }
                    }
                    if (!exists) {
                        ssh_profile_t *pr = &s_profiles[s_profile_count++];
                        memset(pr, 0, sizeof(*pr));
                        strncpy(pr->user, u, sizeof(pr->user) - 1);
                        strncpy(pr->host, h, sizeof(pr->host) - 1);
                        pr->port = pt;
                    }
                }
            }
        }
    }

    /* Ensure Tailscale host shi3z@tsuginosuke is available in saved profiles */
    bool has_tsuginosuke = false;
    for (int i = 0; i < s_profile_count; i++) {
        if (strcmp(s_profiles[i].host, "tsuginosuke") == 0) {
            has_tsuginosuke = true;
            break;
        }
    }
    if (!has_tsuginosuke && s_profile_count < SSH_MAX_PROFILES) {
        ssh_profile_t *pr = &s_profiles[s_profile_count++];
        memset(pr, 0, sizeof(*pr));
        strcpy(pr->user, "shi3z");
        strcpy(pr->host, "tsuginosuke");
        pr->port = 22;
        pr->pass[0] = 0;
        ssh_profiles_save();
    }
}

static void ssh_profiles_ensure_loaded(void)
{
    if (!s_profiles_loaded) {
        ssh_profiles_load();
    }
}

/* Insert or update a profile at index 0 (most recently used) and save to /home/ssh_hosts.cfg */
static void ssh_profiles_upsert(const char *user, const char *host, uint16_t port, const char *pass)
{
    if (!user || !user[0] || !host || !host[0]) return;
    ssh_profiles_ensure_loaded();

    int found = -1;
    for (int i = 0; i < s_profile_count; i++) {
        if (strcmp(s_profiles[i].user, user) == 0 &&
            strcmp(s_profiles[i].host, host) == 0 &&
            s_profiles[i].port == port) {
            found = i;
            break;
        }
    }

    ssh_profile_t entry;
    memset(&entry, 0, sizeof(entry));
    strncpy(entry.user, user, sizeof(entry.user) - 1);
    strncpy(entry.host, host, sizeof(entry.host) - 1);
    entry.port = port ? port : 22;
    if (pass && pass[0]) {
        strncpy(entry.pass, pass, sizeof(entry.pass) - 1);
    } else if (found >= 0) {
        strncpy(entry.pass, s_profiles[found].pass, sizeof(entry.pass) - 1);
    }

    if (found >= 0) {
        for (int i = found; i > 0; i--) {
            s_profiles[i] = s_profiles[i - 1];
        }
        s_profiles[0] = entry;
    } else {
        int limit = (s_profile_count < SSH_MAX_PROFILES) ? s_profile_count : (SSH_MAX_PROFILES - 1);
        for (int i = limit; i > 0; i--) {
            s_profiles[i] = s_profiles[i - 1];
        }
        s_profiles[0] = entry;
        if (s_profile_count < SSH_MAX_PROFILES) s_profile_count++;
    }

    ssh_profiles_save();
}

static const char *ssh_profiles_find_pass(const char *user, const char *host, uint16_t port)
{
    ssh_profiles_ensure_loaded();
    for (int i = 0; i < s_profile_count; i++) {
        if (strcmp(s_profiles[i].user, user) == 0 &&
            strcmp(s_profiles[i].host, host) == 0 &&
            s_profiles[i].port == port &&
            s_profiles[i].pass[0]) {
            return s_profiles[i].pass;
        }
    }
    return NULL;
}

static void ssh_profiles_delete(int idx)
{
    ssh_profiles_ensure_loaded();
    if (idx < 0 || idx >= s_profile_count) return;
    for (int i = idx; i + 1 < s_profile_count; i++) {
        s_profiles[i] = s_profiles[i + 1];
    }
    s_profile_count--;
    ssh_profiles_save();
}

/* =========================================================================
 * 6. Session Channel Helpers & CLI Session Loop
 * ========================================================================= */

static bool ssh_open_channel(ssh_session_t *s, const char *remote_cmd, uint32_t pty_cols, uint32_t pty_rows)
{
    size_t p = 0;
    p = buf_put_u8(s->payload, p, SSH_MSG_CHANNEL_OPEN);
    p = buf_put_str(s->payload, p, "session");
    p = buf_put_u32(s->payload, p, 0);       /* sender channel = 0 */
    p = buf_put_u32(s->payload, p, 65536u);  /* initial window size */
    p = buf_put_u32(s->payload, p, 2048u);   /* max packet size */
    if (!ssh_send_packet(s, s->payload, p)) return false;

    uint8_t *rx = NULL;
    int rx_len = 0;
    for (;;) {
        rx_len = ssh_recv_packet(s, &rx, 10000);
        if (rx_len <= 0) return false;
        if (rx[0] == SSH_MSG_GLOBAL_REQUEST) {
            size_t pos = 1;
            const uint8_t *rname = NULL;
            size_t rlen = 0;
            if (buf_get_str(rx, (size_t)rx_len, &pos, &rname, &rlen) && pos < (size_t)rx_len && rx[pos] != 0) {
                s->payload[0] = SSH_MSG_REQUEST_FAILURE;
                ssh_send_packet(s, s->payload, 1);
            }
            continue;
        }
        if (rx[0] == SSH_MSG_CHANNEL_OPEN_CONFIRMATION) {
            size_t pos = 1;
            uint32_t rcpt = 0, snd = 0, win = 0, maxpkt = 0;
            if (!buf_get_u32(rx, (size_t)rx_len, &pos, &rcpt) ||
                !buf_get_u32(rx, (size_t)rx_len, &pos, &snd) ||
                !buf_get_u32(rx, (size_t)rx_len, &pos, &win) ||
                !buf_get_u32(rx, (size_t)rx_len, &pos, &maxpkt)) {
                return false;
            }
            s->remote_chan = snd;
            s->remote_win  = win;
            break;
        }
        if (rx[0] == SSH_MSG_CHANNEL_OPEN_FAILURE) {
            kprintf("ssh: channel open failed\n");
            return false;
        }
    }

    if (remote_cmd && remote_cmd[0]) {
        p = 0;
        p = buf_put_u8(s->payload, p, SSH_MSG_CHANNEL_REQUEST);
        p = buf_put_u32(s->payload, p, s->remote_chan);
        p = buf_put_str(s->payload, p, "exec");
        p = buf_put_u8(s->payload, p, 1); /* want_reply = TRUE */
        p = buf_put_str(s->payload, p, remote_cmd);
        if (!ssh_send_packet(s, s->payload, p)) return false;
    } else {
        p = 0;
        p = buf_put_u8(s->payload, p, SSH_MSG_CHANNEL_REQUEST);
        p = buf_put_u32(s->payload, p, s->remote_chan);
        p = buf_put_str(s->payload, p, "pty-req");
        p = buf_put_u8(s->payload, p, 0); /* want_reply = FALSE */
        p = buf_put_str(s->payload, p, "xterm-256color");
        p = buf_put_u32(s->payload, p, pty_cols);
        p = buf_put_u32(s->payload, p, pty_rows);
        p = buf_put_u32(s->payload, p, 0);
        p = buf_put_u32(s->payload, p, 0);
        uint8_t modes[1] = { 0 }; /* TTY_OP_END */
        p = buf_put_mem(s->payload, p, modes, 1);
        if (!ssh_send_packet(s, s->payload, p)) return false;

        p = 0;
        p = buf_put_u8(s->payload, p, SSH_MSG_CHANNEL_REQUEST);
        p = buf_put_u32(s->payload, p, s->remote_chan);
        p = buf_put_str(s->payload, p, "env");
        p = buf_put_u8(s->payload, p, 0); /* want_reply = FALSE */
        p = buf_put_str(s->payload, p, "LANG");
        p = buf_put_str(s->payload, p, "C.UTF-8");
        if (!ssh_send_packet(s, s->payload, p)) return false;

        p = 0;
        p = buf_put_u8(s->payload, p, SSH_MSG_CHANNEL_REQUEST);
        p = buf_put_u32(s->payload, p, s->remote_chan);
        p = buf_put_str(s->payload, p, "shell");
        p = buf_put_u8(s->payload, p, 1); /* want_reply = TRUE */
        if (!ssh_send_packet(s, s->payload, p)) return false;
    }
    return true;
}

static int ssh_session_loop(ssh_session_t *s, const char *remote_cmd)
{
    if (!ssh_open_channel(s, remote_cmd, 80, 45)) return 1;
    if (!remote_cmd || !remote_cmd[0]) {
        kprintf("ssh: connected (press ~. after Enter to disconnect)\n");
    }

    bool close_sent = false;
    int tilde_state = 1;
    size_t p = 0;
    uint8_t *rx = NULL;
    int rx_len = 0;

    while (wifi_tcp_is_connected_sock(s->sock)) {
        uint8_t inbuf[64];
        size_t inlen = 0;
        while (inlen < sizeof(inbuf)) {
            int c = kconsole_getc();
            if (c < 0) break;
            if (tilde_state == 1 && c == '~') {
                tilde_state = 2;
                continue;
            }
            if (tilde_state == 2) {
                if (c == '.') {
                    kprintf("\nssh: disconnected by ~.\n");
                    return 0;
                }
                inbuf[inlen++] = '~';
                tilde_state = 0;
            }
            if (c == '\r' || c == '\n') tilde_state = 1;
            else tilde_state = 0;
            inbuf[inlen++] = (uint8_t)c;
        }
        if (inlen > 0 && s->remote_win > 0) {
            p = 0;
            p = buf_put_u8(s->payload, p, SSH_MSG_CHANNEL_DATA);
            p = buf_put_u32(s->payload, p, s->remote_chan);
            p = buf_put_mem(s->payload, p, inbuf, inlen);
            if (!ssh_send_packet(s, s->payload, p)) break;
            if (s->remote_win >= inlen) s->remote_win -= (uint32_t)inlen;
            else s->remote_win = 0;
        }

        rx_len = ssh_recv_packet(s, &rx, 20);
        if (rx_len < 0) break;
        if (rx_len == 0) continue;

        uint8_t msg = rx[0];
        if (msg == SSH_MSG_CHANNEL_DATA || msg == SSH_MSG_CHANNEL_EXTENDED_DATA) {
            size_t pos = 1;
            uint32_t chan = 0, dtype = 0;
            const uint8_t *data = NULL;
            size_t dlen = 0;
            if (!buf_get_u32(rx, (size_t)rx_len, &pos, &chan)) continue;
            if (msg == SSH_MSG_CHANNEL_EXTENDED_DATA && !buf_get_u32(rx, (size_t)rx_len, &pos, &dtype)) continue;
            if (buf_get_str(rx, (size_t)rx_len, &pos, &data, &dlen) && dlen > 0) {
                kconsole_write((const char *)data, dlen);
                s->local_win_consumed += (uint32_t)dlen;
                if (s->local_win_consumed >= 16384u) {
                    p = 0;
                    p = buf_put_u8(s->payload, p, SSH_MSG_CHANNEL_WINDOW_ADJUST);
                    p = buf_put_u32(s->payload, p, s->remote_chan);
                    p = buf_put_u32(s->payload, p, s->local_win_consumed);
                    ssh_send_packet(s, s->payload, p);
                    s->local_win_consumed = 0;
                }
            }
        } else if (msg == SSH_MSG_CHANNEL_WINDOW_ADJUST) {
            size_t pos = 1;
            uint32_t chan = 0, add = 0;
            if (buf_get_u32(rx, (size_t)rx_len, &pos, &chan) &&
                buf_get_u32(rx, (size_t)rx_len, &pos, &add)) {
                s->remote_win += add;
            }
        } else if (msg == SSH_MSG_CHANNEL_REQUEST) {
            size_t pos = 1;
            uint32_t chan = 0;
            const uint8_t *rtype = NULL;
            size_t rlen = 0;
            if (buf_get_u32(rx, (size_t)rx_len, &pos, &chan) &&
                buf_get_str(rx, (size_t)rx_len, &pos, &rtype, &rlen) &&
                pos < (size_t)rx_len) {
                uint8_t want_reply = rx[pos++];
                if (rlen == 11 && memcmp(rtype, "exit-status", 11) == 0) {
                    uint32_t code = 0;
                    if (buf_get_u32(rx, (size_t)rx_len, &pos, &code)) {
                        s->exit_status = (int)code;
                    }
                } else if (want_reply) {
                    p = 0;
                    p = buf_put_u8(s->payload, p, SSH_MSG_CHANNEL_FAILURE);
                    p = buf_put_u32(s->payload, p, s->remote_chan);
                    ssh_send_packet(s, s->payload, p);
                }
            }
        } else if (msg == SSH_MSG_GLOBAL_REQUEST) {
            size_t pos = 1;
            const uint8_t *rname = NULL;
            size_t rlen = 0;
            if (buf_get_str(rx, (size_t)rx_len, &pos, &rname, &rlen) && pos < (size_t)rx_len && rx[pos] != 0) {
                s->payload[0] = SSH_MSG_REQUEST_FAILURE;
                ssh_send_packet(s, s->payload, 1);
            }
        } else if (msg == SSH_MSG_CHANNEL_EOF || msg == SSH_MSG_CHANNEL_CLOSE) {
            if (!close_sent) {
                p = 0;
                p = buf_put_u8(s->payload, p, SSH_MSG_CHANNEL_CLOSE);
                p = buf_put_u32(s->payload, p, s->remote_chan);
                ssh_send_packet(s, s->payload, p);
                close_sent = true;
            }
            if (msg == SSH_MSG_CHANNEL_CLOSE) break;
        }
    }

    return s->exit_status;
}

static bool ssh_ensure_wifi_ready(void)
{
    wifi_status_t st;
    wifi_get_status(&st);

    if (st.state != WIFI_STATE_CONNECTED) {
        char saved_ssid[WIFI_SSID_MAX + 1] = "";
        char saved_pass[WIFI_PASS_MAX + 1] = "";
        bool auto_conn = false;
        if (wifi_load_saved(saved_ssid, saved_pass, &auto_conn) && auto_conn && saved_ssid[0]) {
            kprintf("ssh: connecting to saved Wi-Fi '%s'...\n", saved_ssid);
            wifi_connect(saved_ssid, saved_pass, false);
            wifi_get_status(&st);
        }
    }

    if (st.state == WIFI_STATE_CONNECTED && st.ip[0] == 0) {
        kprintf("ssh: waiting for DHCP IPv4 on '%s'...\n", st.ssid);
        for (int i = 0; i < 60 && st.ip[0] == 0; i++) {
            task_sleep(100);
            wifi_get_status(&st);
        }
    }

    if (st.state != WIFI_STATE_CONNECTED) {
        kprintf("ssh: Wi-Fi is not connected (run 'wifi' first)\n");
        return false;
    }
    if (st.ip[0] == 0) {
        kprintf("ssh: Wi-Fi is associated with '%s', but DHCP IPv4 address is not assigned yet\n", st.ssid);
        return false;
    }
    return true;
}

static int ssh_connect_and_run(const char *user, const char *host, uint16_t port,
                               char *password, size_t pass_cap, const char *remote_cmd)
{
    if (!ssh_ensure_wifi_ready()) return 1;

    /* Look up saved password if none provided on command line */
    if (!password[0]) {
        const char *saved_pw = ssh_profiles_find_pass(user, host, port);
        if (saved_pw && saved_pw[0]) {
            strncpy(password, saved_pw, pass_cap - 1);
            password[pass_cap - 1] = 0;
        }
    }

    uint8_t ip[4] = {0};
    if (!wifi_resolve_host(host, ip)) {
        kprintf("ssh: could not resolve hostname '%s'\n", host);
        return 1;
    }

    kprintf("ssh: connecting to %s@%s (%u.%u.%u.%u:%u)...\n",
            user, host, ip[0], ip[1], ip[2], ip[3], port);

    if (!wifi_tcp_connect_sock(0, ip, port, 8000)) {
        kprintf("ssh: TCP connection to %u.%u.%u.%u:%u failed\n",
                ip[0], ip[1], ip[2], ip[3], port);
        return 1;
    }

    ssh_session_t *s = (ssh_session_t *)kmalloc_psram(sizeof(ssh_session_t));
    if (!s) s = (ssh_session_t *)kmalloc(sizeof(ssh_session_t));
    if (!s) {
        wifi_tcp_close_sock(0);
        kprintf("ssh: out of memory\n");
        return 1;
    }
    memset(s, 0, sizeof(*s));
    s->sock = 0;

    int rc = 1;
    if (ssh_handshake(s)) {
        if (ssh_authenticate(s, user, host, password, pass_cap, false)) {
            ssh_profiles_upsert(user, host, port, password);
            rc = ssh_session_loop(s, remote_cmd);
        }
    }

    memset(password, 0, pass_cap);
    memset(s, 0, sizeof(*s));
    kfree(s);
    wifi_tcp_close_sock(0);
    return rc;
}

/* =========================================================================
 * 7. Multi-Tab Graphical SSH Client + Scrollback Terminal + Host GUI
 * ========================================================================= */

#define SSH_MAX_TABS      WIFI_MAX_TCP_SOCKETS
#define SSH_CELL_PX       (8 * M5OS_FBCON_SCALE)
#define SSH_TOP_BAR_ROWS  2
#define SSH_BOT_BAR_ROWS  2
#define SSH_TERM_COLS     80
#define SSH_TERM_ROWS     41    /* Rows 2..42 on 80x45 display */
#define SSH_HIST_LINES    256   /* Scrollback ring buffer lines per tab */

typedef struct {
    uint16_t ch;
    uint8_t  fg;
    uint8_t  bg;
} term_cell_t;

typedef struct {
    term_cell_t cells[SSH_TERM_COLS];
} term_line_t;

typedef struct {
    int            sock;
    bool           connected;
    char           title[32];
    char           user[32];
    char           host[64];
    uint16_t       port;
    ssh_session_t *s;

    /* Active 80x41 screen + 256-line scrollback history in PSRAM */
    term_line_t    screen[SSH_TERM_ROWS];
    term_line_t   *hist;
    int            hist_head;
    int            hist_count;
    int            scroll_offset;

    /* VT100 / ANSI Escape Sequence + UTF-8 parser state */
    int            cx;
    int            cy;
    uint8_t        cur_fg;
    uint8_t        cur_bg;
    bool           bold;
    bool           reverse;
    int            esc_state;
    int            csi_params[8];
    int            csi_nparams;
    bool           csi_priv;
    uint32_t       utf8_cp;
    uint8_t        utf8_rem;

    /* Per-row dirty flags for fast LCD rendering */
    bool           dirty_rows[SSH_TERM_ROWS];
    bool           all_dirty;
} ssh_tab_t;

static const uint16_t s_ansi_palette[16] = {
    RGB565(0, 0, 0),         /* 0: Black */
    RGB565(205, 49, 49),     /* 1: Red */
    RGB565(13, 188, 121),    /* 2: Green */
    RGB565(229, 229, 16),    /* 3: Yellow */
    RGB565(36, 114, 200),    /* 4: Blue */
    RGB565(188, 63, 188),    /* 5: Magenta */
    RGB565(17, 168, 205),    /* 6: Cyan */
    RGB565(229, 229, 229),   /* 7: White / Light Gray */
    RGB565(102, 102, 102),   /* 8: Bright Black (Dark Gray) */
    RGB565(241, 76, 76),     /* 9: Bright Red */
    RGB565(35, 209, 139),    /* 10: Bright Green */
    RGB565(245, 245, 67),    /* 11: Bright Yellow */
    RGB565(59, 142, 234),    /* 12: Bright Blue */
    RGB565(214, 112, 214),   /* 13: Bright Magenta */
    RGB565(41, 184, 219),    /* 14: Bright Cyan */
    RGB565(255, 255, 255),   /* 15: Bright White */
};

static void term_clear_line(term_line_t *ln, uint8_t fg, uint8_t bg)
{
    for (int x = 0; x < SSH_TERM_COLS; x++) {
        ln->cells[x].ch = ' ';
        ln->cells[x].fg = fg;
        ln->cells[x].bg = bg;
    }
}

static void tab_reset_term(ssh_tab_t *tab)
{
    tab->cx = 0;
    tab->cy = 0;
    tab->cur_fg = 7;
    tab->cur_bg = 0;
    tab->bold = false;
    tab->reverse = false;
    tab->esc_state = 0;
    tab->csi_nparams = 0;
    tab->csi_priv = false;
    tab->utf8_cp = 0;
    tab->utf8_rem = 0;
    tab->hist_head = 0;
    tab->hist_count = 0;
    tab->scroll_offset = 0;
    for (int y = 0; y < SSH_TERM_ROWS; y++) {
        term_clear_line(&tab->screen[y], 7, 0);
        tab->dirty_rows[y] = true;
    }
    tab->all_dirty = true;
}

static void tab_scroll_up(ssh_tab_t *tab)
{
    /* Save top screen line into scrollback ring buffer */
    if (tab->hist) {
        tab->hist[tab->hist_head] = tab->screen[0];
        tab->hist_head = (tab->hist_head + 1) % SSH_HIST_LINES;
        if (tab->hist_count < SSH_HIST_LINES) {
            tab->hist_count++;
        }
        if (tab->scroll_offset > 0 && tab->scroll_offset < tab->hist_count) {
            tab->scroll_offset++;
        }
    }

    for (int y = 0; y < SSH_TERM_ROWS - 1; y++) {
        tab->screen[y] = tab->screen[y + 1];
        tab->dirty_rows[y] = true;
    }
    uint8_t fg = tab->reverse ? tab->cur_bg : (tab->bold ? (uint8_t)(tab->cur_fg | 8u) : tab->cur_fg);
    uint8_t bg = tab->reverse ? (tab->bold ? (uint8_t)(tab->cur_fg | 8u) : tab->cur_fg) : tab->cur_bg;
    term_clear_line(&tab->screen[SSH_TERM_ROWS - 1], fg, bg);
    tab->dirty_rows[SSH_TERM_ROWS - 1] = true;
}

static void tab_newline(ssh_tab_t *tab)
{
    tab->dirty_rows[tab->cy] = true;
    if (tab->cy + 1 < SSH_TERM_ROWS) {
        tab->cy++;
        tab->dirty_rows[tab->cy] = true;
    } else {
        tab_scroll_up(tab);
    }
}

static void tab_handle_sgr(ssh_tab_t *tab)
{
    if (tab->csi_nparams == 0) {
        tab->cur_fg = 7;
        tab->cur_bg = 0;
        tab->bold = false;
        tab->reverse = false;
        return;
    }
    for (int i = 0; i < tab->csi_nparams; i++) {
        int p = tab->csi_params[i];
        if (p == 0) {
            tab->cur_fg = 7;
            tab->cur_bg = 0;
            tab->bold = false;
            tab->reverse = false;
        } else if (p == 1) {
            tab->bold = true;
        } else if (p == 22) {
            tab->bold = false;
        } else if (p == 7) {
            tab->reverse = true;
        } else if (p == 27) {
            tab->reverse = false;
        } else if (p >= 30 && p <= 37) {
            tab->cur_fg = (uint8_t)(p - 30);
        } else if (p == 39) {
            tab->cur_fg = 7;
        } else if (p >= 40 && p <= 47) {
            tab->cur_bg = (uint8_t)(p - 40);
        } else if (p == 49) {
            tab->cur_bg = 0;
        } else if (p >= 90 && p <= 97) {
            tab->cur_fg = (uint8_t)(p - 90 + 8);
        } else if (p >= 100 && p <= 107) {
            tab->cur_bg = (uint8_t)(p - 100 + 8);
        } else if (p == 38 && i + 2 < tab->csi_nparams && tab->csi_params[i + 1] == 5) {
            int c256 = tab->csi_params[i + 2];
            if (c256 >= 0 && c256 < 16) tab->cur_fg = (uint8_t)c256;
            i += 2;
        } else if (p == 48 && i + 2 < tab->csi_nparams && tab->csi_params[i + 1] == 5) {
            int c256 = tab->csi_params[i + 2];
            if (c256 >= 0 && c256 < 16) tab->cur_bg = (uint8_t)c256;
            i += 2;
        }
    }
}

static void tab_handle_csi(ssh_tab_t *tab, char final_ch)
{
    int p0 = (tab->csi_nparams >= 1) ? tab->csi_params[0] : 0;
    int p1 = (tab->csi_nparams >= 2) ? tab->csi_params[1] : 0;
    uint8_t fg = tab->reverse ? tab->cur_bg : (tab->bold && tab->cur_fg < 8 ? (uint8_t)(tab->cur_fg + 8) : tab->cur_fg);
    uint8_t bg = tab->reverse ? (tab->bold && tab->cur_fg < 8 ? (uint8_t)(tab->cur_fg + 8) : tab->cur_fg) : tab->cur_bg;

    tab->dirty_rows[tab->cy] = true;

    switch (final_ch) {
    case 'm':
        tab_handle_sgr(tab);
        break;
    case 'A': { /* Cursor Up */
        int n = (p0 > 0) ? p0 : 1;
        tab->cy -= n;
        if (tab->cy < 0) tab->cy = 0;
        tab->dirty_rows[tab->cy] = true;
        break;
    }
    case 'B': { /* Cursor Down */
        int n = (p0 > 0) ? p0 : 1;
        tab->cy += n;
        if (tab->cy >= SSH_TERM_ROWS) tab->cy = SSH_TERM_ROWS - 1;
        tab->dirty_rows[tab->cy] = true;
        break;
    }
    case 'C': { /* Cursor Forward */
        int n = (p0 > 0) ? p0 : 1;
        tab->cx += n;
        if (tab->cx >= SSH_TERM_COLS) tab->cx = SSH_TERM_COLS - 1;
        break;
    }
    case 'D': { /* Cursor Back */
        int n = (p0 > 0) ? p0 : 1;
        tab->cx -= n;
        if (tab->cx < 0) tab->cx = 0;
        break;
    }
    case 'G': { /* Cursor Horizontal Absolute */
        int col = (p0 > 0) ? (p0 - 1) : 0;
        if (col < 0) col = 0;
        if (col >= SSH_TERM_COLS) col = SSH_TERM_COLS - 1;
        tab->cx = col;
        break;
    }
    case 'd': { /* Line Position Absolute */
        int row = (p0 > 0) ? (p0 - 1) : 0;
        if (row < 0) row = 0;
        if (row >= SSH_TERM_ROWS) row = SSH_TERM_ROWS - 1;
        tab->cy = row;
        tab->dirty_rows[tab->cy] = true;
        break;
    }
    case 'H':
    case 'f': { /* Cursor Position */
        int row = (p0 > 0) ? (p0 - 1) : 0;
        int col = (p1 > 0) ? (p1 - 1) : 0;
        if (row < 0) row = 0;
        if (row >= SSH_TERM_ROWS) row = SSH_TERM_ROWS - 1;
        if (col < 0) col = 0;
        if (col >= SSH_TERM_COLS) col = SSH_TERM_COLS - 1;
        tab->cy = row;
        tab->cx = col;
        tab->dirty_rows[tab->cy] = true;
        break;
    }
    case 'J': { /* Erase in Display */
        if (p0 == 2 || p0 == 3) {
            for (int y = 0; y < SSH_TERM_ROWS; y++) {
                term_clear_line(&tab->screen[y], fg, bg);
                tab->dirty_rows[y] = true;
            }
            tab->cx = 0;
            tab->cy = 0;
        } else if (p0 == 0) {
            for (int x = tab->cx; x < SSH_TERM_COLS; x++) {
                tab->screen[tab->cy].cells[x].ch = ' ';
                tab->screen[tab->cy].cells[x].fg = fg;
                tab->screen[tab->cy].cells[x].bg = bg;
            }
            tab->dirty_rows[tab->cy] = true;
            for (int y = tab->cy + 1; y < SSH_TERM_ROWS; y++) {
                term_clear_line(&tab->screen[y], fg, bg);
                tab->dirty_rows[y] = true;
            }
        } else if (p0 == 1) {
            for (int y = 0; y < tab->cy; y++) {
                term_clear_line(&tab->screen[y], fg, bg);
                tab->dirty_rows[y] = true;
            }
            for (int x = 0; x <= tab->cx && x < SSH_TERM_COLS; x++) {
                tab->screen[tab->cy].cells[x].ch = ' ';
                tab->screen[tab->cy].cells[x].fg = fg;
                tab->screen[tab->cy].cells[x].bg = bg;
            }
            tab->dirty_rows[tab->cy] = true;
        }
        break;
    }
    case 'K': { /* Erase in Line */
        if (p0 == 0) {
            for (int x = tab->cx; x < SSH_TERM_COLS; x++) {
                tab->screen[tab->cy].cells[x].ch = ' ';
                tab->screen[tab->cy].cells[x].fg = fg;
                tab->screen[tab->cy].cells[x].bg = bg;
            }
        } else if (p0 == 1) {
            for (int x = 0; x <= tab->cx && x < SSH_TERM_COLS; x++) {
                tab->screen[tab->cy].cells[x].ch = ' ';
                tab->screen[tab->cy].cells[x].fg = fg;
                tab->screen[tab->cy].cells[x].bg = bg;
            }
        } else if (p0 == 2) {
            term_clear_line(&tab->screen[tab->cy], fg, bg);
        }
        tab->dirty_rows[tab->cy] = true;
        break;
    }
    case 'P': { /* Delete Character */
        int n = (p0 > 0) ? p0 : 1;
        if (n > SSH_TERM_COLS - tab->cx) n = SSH_TERM_COLS - tab->cx;
        for (int x = tab->cx; x + n < SSH_TERM_COLS; x++) {
            tab->screen[tab->cy].cells[x] = tab->screen[tab->cy].cells[x + n];
        }
        for (int x = SSH_TERM_COLS - n; x < SSH_TERM_COLS; x++) {
            tab->screen[tab->cy].cells[x].ch = ' ';
            tab->screen[tab->cy].cells[x].fg = fg;
            tab->screen[tab->cy].cells[x].bg = bg;
        }
        tab->dirty_rows[tab->cy] = true;
        break;
    }
    case '@': { /* Insert Blank Characters */
        int n = (p0 > 0) ? p0 : 1;
        if (n > SSH_TERM_COLS - tab->cx) n = SSH_TERM_COLS - tab->cx;
        for (int x = SSH_TERM_COLS - 1; x >= tab->cx + n; x--) {
            tab->screen[tab->cy].cells[x] = tab->screen[tab->cy].cells[x - n];
        }
        for (int x = tab->cx; x < tab->cx + n && x < SSH_TERM_COLS; x++) {
            tab->screen[tab->cy].cells[x].ch = ' ';
            tab->screen[tab->cy].cells[x].fg = fg;
            tab->screen[tab->cy].cells[x].bg = bg;
        }
        tab->dirty_rows[tab->cy] = true;
        break;
    }
    case 'X': { /* Erase Characters */
        int n = (p0 > 0) ? p0 : 1;
        for (int x = tab->cx; x < tab->cx + n && x < SSH_TERM_COLS; x++) {
            tab->screen[tab->cy].cells[x].ch = ' ';
            tab->screen[tab->cy].cells[x].fg = fg;
            tab->screen[tab->cy].cells[x].bg = bg;
        }
        tab->dirty_rows[tab->cy] = true;
        break;
    }
    default:
        break;
    }
}

static void tab_put_codepoint(ssh_tab_t *tab, uint32_t cp)
{
    if (tab->cx >= SSH_TERM_COLS) {
        tab->cx = 0;
        tab_newline(tab);
    }
    uint8_t fg = tab->reverse ? tab->cur_bg : (tab->bold && tab->cur_fg < 8 ? (uint8_t)(tab->cur_fg + 8) : tab->cur_fg);
    uint8_t bg = tab->reverse ? (tab->bold && tab->cur_fg < 8 ? (uint8_t)(tab->cur_fg + 8) : tab->cur_fg) : tab->cur_bg;
    tab->screen[tab->cy].cells[tab->cx].ch = (uint16_t)(cp <= 0xFFFFu ? cp : 0xFFFDu);
    tab->screen[tab->cy].cells[tab->cx].fg = fg;
    tab->screen[tab->cy].cells[tab->cx].bg = bg;
    tab->dirty_rows[tab->cy] = true;
    tab->cx++;
}

static void tab_feed_byte(ssh_tab_t *tab, uint8_t b)
{
    /* State 0: Normal characters */
    if (tab->esc_state == 0) {
        if (b == 0x1B) {
            tab->utf8_rem = 0;
            tab->esc_state = 1;
            return;
        }
        if (b >= 0x80u) {
            if ((b & 0xC0u) == 0x80u) {
                if (tab->utf8_rem > 0) {
                    tab->utf8_cp = (tab->utf8_cp << 6) | (uint32_t)(b & 0x3Fu);
                    tab->utf8_rem--;
                    if (tab->utf8_rem == 0) {
                        tab_put_codepoint(tab, tab->utf8_cp);
                    }
                }
            } else if ((b & 0xE0u) == 0xC0u) {
                tab->utf8_cp  = (uint32_t)(b & 0x1Fu);
                tab->utf8_rem = 1;
            } else if ((b & 0xF0u) == 0xE0u) {
                tab->utf8_cp  = (uint32_t)(b & 0x0Fu);
                tab->utf8_rem = 2;
            } else if ((b & 0xF8u) == 0xF0u) {
                tab->utf8_cp  = (uint32_t)(b & 0x07u);
                tab->utf8_rem = 3;
            } else {
                tab->utf8_rem = 0;
            }
            return;
        }
        tab->utf8_rem = 0;
        if (b == '\r') {
            tab->cx = 0;
            tab->dirty_rows[tab->cy] = true;
            return;
        }
        if (b == '\n') {
            tab_newline(tab);
            return;
        }
        if (b == '\b' || b == 0x08) {
            if (tab->cx > 0) {
                tab->cx--;
                tab->dirty_rows[tab->cy] = true;
            }
            return;
        }
        if (b == '\t') {
            int next_tab = (tab->cx + 8) & ~7;
            if (next_tab >= SSH_TERM_COLS) next_tab = SSH_TERM_COLS - 1;
            tab->cx = next_tab;
            tab->dirty_rows[tab->cy] = true;
            return;
        }
        if (b < 0x20 || b == 0x7F) {
            return;
        }
        tab_put_codepoint(tab, (uint32_t)b);
        return;
    }

    /* State 1: Just saw ESC */
    if (tab->esc_state == 1) {
        if (b == '[') {
            tab->esc_state = 2;
            tab->csi_nparams = 0;
            tab->csi_priv = false;
            for (int i = 0; i < 8; i++) tab->csi_params[i] = 0;
            return;
        }
        if (b == ']') { /* OSC sequence: skip until BEL (0x07) or ST (ESC \) */
            tab->esc_state = 3;
            return;
        }
        if (b == '(' || b == ')' || b == '*' || b == '+') { /* Charset designation: skip 1 byte */
            tab->esc_state = 4;
            return;
        }
        if (b == 'c') { /* Full reset */
            tab_reset_term(tab);
            return;
        }
        if (b == 'D' || b == 'E') {
            if (b == 'E') tab->cx = 0;
            tab_newline(tab);
            tab->esc_state = 0;
            return;
        }
        if (b == 'M') { /* Reverse index */
            if (tab->cy > 0) tab->cy--;
            tab->dirty_rows[tab->cy] = true;
            tab->esc_state = 0;
            return;
        }
        tab->esc_state = 0;
        return;
    }

    /* State 2: Inside CSI (ESC [ ...) */
    if (tab->esc_state == 2) {
        if (b == '?' || b == '>' || b == '=') {
            tab->csi_priv = true;
            return;
        }
        if (b >= '0' && b <= '9') {
            if (tab->csi_nparams == 0) tab->csi_nparams = 1;
            int idx = tab->csi_nparams - 1;
            if (idx < 8) {
                tab->csi_params[idx] = tab->csi_params[idx] * 10 + (b - '0');
            }
            return;
        }
        if (b == ';') {
            if (tab->csi_nparams == 0) tab->csi_nparams = 1;
            if (tab->csi_nparams < 8) {
                tab->csi_nparams++;
                tab->csi_params[tab->csi_nparams - 1] = 0;
            }
            return;
        }
        if (b >= 0x40 && b <= 0x7E) {
            tab_handle_csi(tab, (char)b);
            tab->esc_state = 0;
            return;
        }
        return;
    }

    /* State 3: Inside OSC (ESC ] ...) */
    if (tab->esc_state == 3) {
        if (b == 0x07) {
            tab->esc_state = 0;
        } else if (b == 0x1B) {
            tab->esc_state = 4;
        }
        return;
    }

    /* State 4: Consume 1 trailing byte */
    tab->esc_state = 0;
}

static void tab_feed_str(ssh_tab_t *tab, const char *s)
{
    if (!s) return;
    while (*s) {
        tab_feed_byte(tab, (uint8_t)*s++);
    }
}

static void gui_draw_padded(int cx, int cy, int width_cells, const char *s, uint16_t fg, uint16_t bg)
{
    char line[128];
    int n = 0;
    if (width_cells > (int)sizeof(line) - 1) width_cells = (int)sizeof(line) - 1;
    while (s && s[n] && n < width_cells) {
        line[n] = s[n];
        n++;
    }
    while (n < width_cells) line[n++] = ' ';
    line[n] = 0;
    display_draw_text(cx * SSH_CELL_PX, cy * SSH_CELL_PX, line, fg, bg, M5OS_FBCON_SCALE);
}

static void gui_show_status_box(const char *line1, const char *line2)
{
    int w = 56;
    int h = 6;
    int x = (SSH_TERM_COLS - w) / 2;
    int y = (dq_rows() - h) / 2;
    dq_window(x, y, w, h);
    gui_draw_padded(x + 2, y + 2, w - 4, line1, COLOR_YELLOW, COLOR_BLACK);
    if (line2) {
        gui_draw_padded(x + 2, y + 3, w - 4, line2, COLOR_CYAN, COLOR_BLACK);
    }
}

/* Retrieve line `row` (0 .. SSH_TERM_ROWS-1) taking `tab->scroll_offset` into account */
static const term_line_t *tab_get_view_line(const ssh_tab_t *tab, int row)
{
    /*
     * Total lines available = tab->hist_count (oldest..newest in hist) + SSH_TERM_ROWS (live screen).
     * At scroll_offset == 0, view row `r` is `tab->screen[r]`.
     * At scroll_offset > 0, virtual index `v = (tab->hist_count - tab->scroll_offset) + row`.
     */
    int v = (tab->hist_count - tab->scroll_offset) + row;
    if (v < 0) v = 0;
    if (v < tab->hist_count && tab->hist) {
        int oldest = (tab->hist_head - tab->hist_count + SSH_HIST_LINES) % SSH_HIST_LINES;
        int hidx   = (oldest + v) % SSH_HIST_LINES;
        return &tab->hist[hidx];
    }
    int srow = v - tab->hist_count;
    if (srow < 0) srow = 0;
    if (srow >= SSH_TERM_ROWS) srow = SSH_TERM_ROWS - 1;
    return &tab->screen[srow];
}

static void gui_render_bars(ssh_tab_t tabs[SSH_MAX_TABS], int active_tab, bool show_manager, bool ctrl_b_prefix)
{
    /* Top Bar Row 0: Title + Tab Strip */
    gui_draw_padded(0, 0, SSH_TERM_COLS, "", COLOR_WHITE, RGB565(18, 28, 56));
    gui_draw_padded(1, 0, 10, "MeOS SSH", RGB565(255, 215, 0), RGB565(18, 28, 56));

    int cx = 11;
    for (int t = 0; t < SSH_MAX_TABS; t++) {
        char badge[20];
        const char *name = tabs[t].connected ? tabs[t].title : (tabs[t].hist_count > 0 ? tabs[t].title : "empty");
        if (!name[0]) name = "empty";
        char short_name[11];
        strncpy(short_name, name, 10);
        short_name[10] = 0;
        ksnprintf(badge, sizeof(badge), " %d:%c%-10s ",
                  t + 1,
                  tabs[t].connected ? '*' : '-',
                  short_name);
        uint16_t fg = (t == active_tab) ? COLOR_BLACK : (tabs[t].connected ? COLOR_GREEN : RGB565(170, 180, 200));
        uint16_t bg = (t == active_tab) ? RGB565(255, 215, 0) : RGB565(32, 46, 84);
        gui_draw_padded(cx, 0, 15, badge, fg, bg);
        cx += 16;
    }

    const ssh_tab_t *act = &tabs[active_tab];
    if (show_manager) {
        gui_draw_padded(75, 0, 5, "[GUI]", COLOR_BLACK, COLOR_CYAN);
    } else if (act->scroll_offset > 0) {
        gui_draw_padded(75, 0, 5, "[SCR]", COLOR_BLACK, COLOR_YELLOW);
    } else if (act->connected) {
        gui_draw_padded(75, 0, 5, "[SSH]", COLOR_BLACK, COLOR_GREEN);
    } else {
        gui_draw_padded(75, 0, 5, "[IDLE]", COLOR_WHITE, RGB565(80, 80, 80));
    }

    /* Top Bar Row 1: Separator / Active Target & Scrollback Status */
    char sub[84];
    if (act->connected) {
        if (act->scroll_offset > 0) {
            ksnprintf(sub, sizeof(sub), " Tab %d: %s@%s:%u (Connected) | SCROLLBACK -%d/%d lines (Enter/Esc: Live)",
                      active_tab + 1, act->user, act->host, (unsigned)act->port,
                      act->scroll_offset, act->hist_count);
        } else {
            ksnprintf(sub, sizeof(sub), " Tab %d: %s@%s:%u (Connected) | History: %d lines",
                      active_tab + 1, act->user, act->host, (unsigned)act->port, act->hist_count);
        }
    } else {
        ksnprintf(sub, sizeof(sub), " Tab %d: Disconnected | Saved Profiles: %d (%s)",
                  active_tab + 1, s_profile_count, SSH_HOSTS_CFG_PATH);
    }
    gui_draw_padded(0, 1, SSH_TERM_COLS, sub, RGB565(180, 220, 255), RGB565(10, 16, 34));

    /* Bottom Bar Rows 43..44 */
    if (show_manager) {
        gui_draw_padded(0, 43, SSH_TERM_COLS,
                        " [Up/Dn] Select  [Enter] Connect Tab  [T] Connect New Tab  [1-4/Tab] Switch Tab",
                        COLOR_YELLOW, RGB565(18, 28, 56));
        gui_draw_padded(0, 44, SSH_TERM_COLS,
                        " [N] New Host  [E] Edit  [D] Delete  [P] Show Pass  [K] Disc  [Space] Term  [Q] Quit",
                        COLOR_CYAN, RGB565(12, 18, 38));
    } else if (act->scroll_offset > 0) {
        gui_draw_padded(0, 43, SSH_TERM_COLS,
                        " SCROLL MODE: [Up/Dn/k/j] Line  [PgUp/PgDn/u/d] Page  [Home/End] Top/Bottom",
                        COLOR_BLACK, COLOR_YELLOW);
        gui_draw_padded(0, 44, SSH_TERM_COLS,
                        " [Enter/Esc/q] Return to Live Terminal   [^O] Host GUI   [^T / F1-F4] Switch Tab",
                        COLOR_WHITE, RGB565(18, 28, 56));
    } else {
        char bot1[84];
        if (ctrl_b_prefix) {
            ksnprintf(bot1, sizeof(bot1),
                      " ^\\ PREFIX: [1-4] Tab 1-4  [n/p] Next/Prev  [o/c] Host GUI  [u/[] Scroll  [x] Disc  [q] Quit");
            gui_draw_padded(0, 43, SSH_TERM_COLS, bot1, COLOR_BLACK, COLOR_CYAN);
        } else {
            ksnprintf(bot1, sizeof(bot1),
                      " [^O] Host Manager GUI   [^T / F1-F4] Switch Tab   [PgUp] Scroll History (%d lines)",
                      act->hist_count);
            gui_draw_padded(0, 43, SSH_TERM_COLS, bot1, COLOR_YELLOW, RGB565(18, 28, 56));
        }
        gui_draw_padded(0, 44, SSH_TERM_COLS,
                        " [^B passes to remote tmux]   [^O] Host GUI   [~. after Enter] Disconnect Tab",
                        COLOR_CYAN, RGB565(12, 18, 38));
    }
}

static void gui_render_terminal(ssh_tab_t *tab)
{
    bool show_cursor = (tab->scroll_offset == 0 && tab->connected);
    for (int r = 0; r < SSH_TERM_ROWS; r++) {
        if (!tab->all_dirty && !tab->dirty_rows[r]) continue;
        const term_line_t *ln = tab_get_view_line(tab, r);
        int screen_y = (SSH_TOP_BAR_ROWS + r) * SSH_CELL_PX;

        /* Draw contiguous runs of identical (fg, bg) on row `r` for fast rendering */
        int x = 0;
        while (x < SSH_TERM_COLS) {
            bool is_cur = (show_cursor && r == tab->cy && x == tab->cx);
            uint8_t fg_idx = ln->cells[x].fg & 15u;
            uint8_t bg_idx = ln->cells[x].bg & 15u;
            uint16_t fg = is_cur ? s_ansi_palette[bg_idx] : s_ansi_palette[fg_idx];
            uint16_t bg = is_cur ? COLOR_GREEN            : s_ansi_palette[bg_idx];

            uint16_t run[SSH_TERM_COLS];
            int run_len = 0;
            int start_x = x;

            while (x < SSH_TERM_COLS) {
                bool cur_here = (show_cursor && r == tab->cy && x == tab->cx);
                uint8_t f2 = ln->cells[x].fg & 15u;
                uint8_t b2 = ln->cells[x].bg & 15u;
                uint16_t fg2 = cur_here ? s_ansi_palette[b2] : s_ansi_palette[f2];
                uint16_t bg2 = cur_here ? COLOR_GREEN         : s_ansi_palette[b2];
                if (fg2 != fg || bg2 != bg) break;
                uint16_t ch = ln->cells[x].ch;
                run[run_len++] = (ch >= 0x20u && ch != 0x7Fu) ? ch : ' ';
                x++;
            }
            display_draw_codepoints(start_x * SSH_CELL_PX, screen_y, run, run_len, fg, bg, M5OS_FBCON_SCALE);
        }
        tab->dirty_rows[r] = false;
    }
    tab->all_dirty = false;
}

static void gui_render_manager(ssh_tab_t tabs[SSH_MAX_TABS], int active_tab, int sel_idx, bool show_passwords)
{
    /* Clear viewport rows 2..42 */
    display_fill_rect(0, SSH_TOP_BAR_ROWS * SSH_CELL_PX,
                      DISPLAY_WIDTH, SSH_TERM_ROWS * SSH_CELL_PX, COLOR_BLACK);

    /* Wi-Fi Status Banner (Rows 3..5) */
    wifi_status_t wst;
    wifi_get_status(&wst);
    dq_window(1, 3, 78, 4);
    char wline[76];
    if (wst.state == WIFI_STATE_CONNECTED && wst.ip[0]) {
        ksnprintf(wline, sizeof(wline), "Wi-Fi: Connected to \"%s\"  IP: %u.%u.%u.%u  ([W] Wi-Fi GUI)",
                  wst.ssid, wst.ip[0], wst.ip[1], wst.ip[2], wst.ip[3]);
        gui_draw_padded(3, 4, 74, wline, COLOR_GREEN, COLOR_BLACK);
    } else {
        ksnprintf(wline, sizeof(wline), "Wi-Fi: Disconnected  (Press [W] to open Wi-Fi Setup GUI)");
        gui_draw_padded(3, 4, 74, wline, COLOR_YELLOW, COLOR_BLACK);
    }
    char tline[76];
    if (tabs[active_tab].connected) {
        ksnprintf(tline, sizeof(tline), "Active Tab %d: Connected to %s@%s:%u  ([Space] View Terminal  [K] Disconnect)",
                  active_tab + 1, tabs[active_tab].user, tabs[active_tab].host, (unsigned)tabs[active_tab].port);
        gui_draw_padded(3, 5, 74, tline, COLOR_CYAN, COLOR_BLACK);
    } else {
        ksnprintf(tline, sizeof(tline), "Active Tab %d: Empty / Ready  (Select a host below and press [Enter] or [T])",
                  active_tab + 1);
        gui_draw_padded(3, 5, 74, tline, RGB565(200, 200, 200), COLOR_BLACK);
    }

    /* Saved Connections List Window (Rows 7..41) */
    dq_window(1, 7, 78, 35);
    gui_draw_padded(3, 8, 74,
                    "SAVED SSH CONNECTIONS & PASSWORDS (/home/ssh_hosts.cfg)",
                    RGB565(255, 215, 0), COLOR_BLACK);
    gui_draw_padded(3, 10, 74,
                    " #   USER @ HOST : PORT                  PASSWORD          OPEN TAB",
                    COLOR_CYAN, RGB565(20, 30, 55));

    if (s_profile_count == 0) {
        gui_draw_padded(5, 13, 68, "No saved SSH connections yet. Press [N] to register a host & password.",
                        RGB565(180, 180, 180), COLOR_BLACK);
        return;
    }

    int max_vis = 14;
    int top = 0;
    if (sel_idx >= max_vis) top = sel_idx - max_vis + 1;

    for (int i = 0; i < max_vis && (top + i) < s_profile_count; i++) {
        int idx = top + i;
        const ssh_profile_t *pr = &s_profiles[idx];
        int ry = 12 + i * 2;
        bool sel = (idx == sel_idx);

        char target_str[38];
        if (pr->port != 22) {
            ksnprintf(target_str, sizeof(target_str), "%s@%s:%u", pr->user, pr->host, (unsigned)pr->port);
        } else {
            ksnprintf(target_str, sizeof(target_str), "%s@%s", pr->user, pr->host);
        }

        char pass_str[18];
        if (!pr->pass[0]) {
            strcpy(pass_str, "(ssh-key)");
        } else if (show_passwords) {
            strncpy(pass_str, pr->pass, 16);
            pass_str[16] = 0;
        } else {
            size_t plen = strlen(pr->pass);
            if (plen > 12) plen = 12;
            for (size_t k = 0; k < plen; k++) pass_str[k] = '*';
            pass_str[plen] = 0;
        }

        /* Check if this profile is currently open in any tab */
        char tab_badge[16] = "-";
        for (int t = 0; t < SSH_MAX_TABS; t++) {
            if (tabs[t].connected &&
                strcmp(tabs[t].user, pr->user) == 0 &&
                strcmp(tabs[t].host, pr->host) == 0 &&
                tabs[t].port == pr->port) {
                ksnprintf(tab_badge, sizeof(tab_badge), "[Tab %d *]", t + 1);
                break;
            }
        }

        char row_str[76];
        ksnprintf(row_str, sizeof(row_str), "%c%2d  %-34s  %-16s  %-10s",
                  sel ? '>' : ' ',
                  idx + 1,
                  target_str,
                  pass_str,
                  tab_badge);

        uint16_t fg = sel ? COLOR_BLACK : COLOR_WHITE;
        uint16_t bg = sel ? RGB565(255, 215, 0) : COLOR_BLACK;
        gui_draw_padded(3, ry, 74, row_str, fg, bg);
    }
}

static void tab_disconnect(ssh_tab_t *tab, const char *reason)
{
    if (tab->s) {
        if (tab->connected) {
            size_t p = 0;
            p = buf_put_u8(tab->s->payload, p, SSH_MSG_CHANNEL_CLOSE);
            p = buf_put_u32(tab->s->payload, p, tab->s->remote_chan);
            ssh_send_packet(tab->s, tab->s->payload, p);
        }
        memset(tab->s, 0, sizeof(*tab->s));
        kfree(tab->s);
        tab->s = NULL;
    }
    wifi_tcp_close_sock(tab->sock);
    if (tab->connected && reason && reason[0]) {
        tab_feed_str(tab, "\r\n\x1b[1;33m[");
        tab_feed_str(tab, reason);
        tab_feed_str(tab, "]\x1b[0m\r\n");
    }
    tab->connected = false;
}

static bool tab_connect_profile(ssh_tab_t *tab, const char *user, const char *host, uint16_t port,
                                const char *initial_pass)
{
    tab_disconnect(tab, NULL);
    tab_reset_term(tab);

    strncpy(tab->user, user, sizeof(tab->user) - 1);
    strncpy(tab->host, host, sizeof(tab->host) - 1);
    tab->port = port ? port : 22;
    ksnprintf(tab->title, sizeof(tab->title), "%s@%s", tab->user, tab->host);

    char pass[64] = "";
    if (initial_pass && initial_pass[0]) {
        strncpy(pass, initial_pass, sizeof(pass) - 1);
    } else {
        const char *saved = ssh_profiles_find_pass(tab->user, tab->host, tab->port);
        if (saved && saved[0]) {
            strncpy(pass, saved, sizeof(pass) - 1);
        }
    }

    gui_show_status_box("Checking Wi-Fi & resolving host...", tab->title);
    if (!ssh_ensure_wifi_ready()) {
        dq_message("Wi-Fi is not connected.", "Press [W] in SSH Manager to configure Wi-Fi.");
        return false;
    }

    uint8_t ip[4] = {0};
    if (!wifi_resolve_host(tab->host, ip)) {
        dq_message("DNS resolution failed for host:", tab->host);
        return false;
    }

    char msg2[64];
    ksnprintf(msg2, sizeof(msg2), "%s@%u.%u.%u.%u:%u (Tab %d)",
              tab->user, ip[0], ip[1], ip[2], ip[3], (unsigned)tab->port, tab->sock + 1);
    gui_show_status_box("Connecting TCP & SSH-2 curve25519...", msg2);

    if (!wifi_tcp_connect_sock(tab->sock, ip, tab->port, 8000)) {
        dq_message("TCP connection failed:", msg2);
        return false;
    }

    ssh_session_t *s = (ssh_session_t *)kmalloc_psram(sizeof(ssh_session_t));
    if (!s) s = (ssh_session_t *)kmalloc(sizeof(ssh_session_t));
    if (!s) {
        wifi_tcp_close_sock(tab->sock);
        dq_message("Out of memory allocating SSH session", NULL);
        return false;
    }
    memset(s, 0, sizeof(*s));
    s->sock = tab->sock;
    tab->s  = s;

    if (!ssh_handshake(s)) {
        tab_disconnect(tab, NULL);
        dq_message("SSH-2 key exchange failed:", msg2);
        return false;
    }

    gui_show_status_box("Authenticating SSH user (ssh-ed25519 / password)...", msg2);
    if (!ssh_authenticate(s, tab->user, tab->host, pass, sizeof(pass), true)) {
        tab_disconnect(tab, NULL);
        dq_message("SSH authentication failed:", tab->title);
        return false;
    }

    if (!ssh_open_channel(s, NULL, SSH_TERM_COLS, SSH_TERM_ROWS)) {
        tab_disconnect(tab, NULL);
        dq_message("Failed to open SSH PTY shell channel", tab->title);
        return false;
    }

    /* Save/update profile & password in /home/ssh_hosts.cfg */
    ssh_profiles_upsert(tab->user, tab->host, tab->port, pass);
    memset(pass, 0, sizeof(pass));

    tab->connected = true;
    tab->scroll_offset = 0;
    tab->all_dirty = true;

    char banner[96];
    ksnprintf(banner, sizeof(banner),
              "\x1b[1;32m[Connected to %s@%s:%u in Tab %d]\x1b[0m\r\n",
              tab->user, tab->host, (unsigned)tab->port, tab->sock + 1);
    tab_feed_str(tab, banner);
    return true;
}

static bool tab_send_bytes(ssh_tab_t *tab, const void *data, size_t len)
{
    if (!tab || !tab->connected || !tab->s || len == 0) return false;
    if (tab->s->remote_win == 0) return true;
    size_t send_len = len;
    if (send_len > tab->s->remote_win) send_len = tab->s->remote_win;

    size_t p = 0;
    p = buf_put_u8(tab->s->payload, p, SSH_MSG_CHANNEL_DATA);
    p = buf_put_u32(tab->s->payload, p, tab->s->remote_chan);
    p = buf_put_mem(tab->s->payload, p, data, send_len);
    if (!ssh_send_packet(tab->s, tab->s->payload, p)) {
        tab_disconnect(tab, "Connection lost while sending");
        return false;
    }
    tab->s->remote_win -= (uint32_t)send_len;
    return true;
}

/* Poll up to `max_pkts` incoming SSH packets on `tab` with `timeout_ms` on the first read */
static bool tab_poll_rx(ssh_tab_t *tab, bool is_active_tab, uint32_t first_timeout_ms)
{
    if (!tab->connected || !tab->s) return false;
    bool got_output = false;

    for (int pkt_i = 0; pkt_i < 6; pkt_i++) {
        uint8_t *rx = NULL;
        uint32_t tmo = (pkt_i == 0) ? first_timeout_ms : 0;
        int rx_len = ssh_recv_packet(tab->s, &rx, tmo);
        if (rx_len < 0) {
            tab_disconnect(tab, "Remote host closed SSH connection");
            return true;
        }
        if (rx_len == 0) {
            if (!wifi_tcp_is_connected_sock(tab->sock)) {
                tab_disconnect(tab, "TCP connection closed by remote host");
                return true;
            }
            break;
        }

        uint8_t msg = rx[0];
        if (msg == SSH_MSG_CHANNEL_DATA || msg == SSH_MSG_CHANNEL_EXTENDED_DATA) {
            size_t pos = 1;
            uint32_t chan = 0, dtype = 0;
            const uint8_t *data = NULL;
            size_t dlen = 0;
            if (!buf_get_u32(rx, (size_t)rx_len, &pos, &chan)) continue;
            if (msg == SSH_MSG_CHANNEL_EXTENDED_DATA && !buf_get_u32(rx, (size_t)rx_len, &pos, &dtype)) continue;
            if (buf_get_str(rx, (size_t)rx_len, &pos, &data, &dlen) && dlen > 0) {
                for (size_t i = 0; i < dlen; i++) {
                    tab_feed_byte(tab, data[i]);
                }
                if (is_active_tab) {
                    /* Also mirror to serial console when fbcon is paused */
                    kconsole_write((const char *)data, dlen);
                }
                got_output = true;
                tab->s->local_win_consumed += (uint32_t)dlen;
                if (tab->s->local_win_consumed >= 16384u) {
                    size_t p = 0;
                    p = buf_put_u8(tab->s->payload, p, SSH_MSG_CHANNEL_WINDOW_ADJUST);
                    p = buf_put_u32(tab->s->payload, p, tab->s->remote_chan);
                    p = buf_put_u32(tab->s->payload, p, tab->s->local_win_consumed);
                    ssh_send_packet(tab->s, tab->s->payload, p);
                    tab->s->local_win_consumed = 0;
                }
            }
        } else if (msg == SSH_MSG_CHANNEL_WINDOW_ADJUST) {
            size_t pos = 1;
            uint32_t chan = 0, add = 0;
            if (buf_get_u32(rx, (size_t)rx_len, &pos, &chan) &&
                buf_get_u32(rx, (size_t)rx_len, &pos, &add)) {
                tab->s->remote_win += add;
            }
        } else if (msg == SSH_MSG_CHANNEL_REQUEST) {
            size_t pos = 1;
            uint32_t chan = 0;
            const uint8_t *rtype = NULL;
            size_t rlen = 0;
            if (buf_get_u32(rx, (size_t)rx_len, &pos, &chan) &&
                buf_get_str(rx, (size_t)rx_len, &pos, &rtype, &rlen) &&
                pos < (size_t)rx_len) {
                uint8_t want_reply = rx[pos++];
                if (rlen == 11 && memcmp(rtype, "exit-status", 11) == 0) {
                    uint32_t code = 0;
                    if (buf_get_u32(rx, (size_t)rx_len, &pos, &code)) {
                        tab->s->exit_status = (int)code;
                    }
                } else if (want_reply) {
                    size_t p = 0;
                    p = buf_put_u8(tab->s->payload, p, SSH_MSG_CHANNEL_FAILURE);
                    p = buf_put_u32(tab->s->payload, p, tab->s->remote_chan);
                    ssh_send_packet(tab->s, tab->s->payload, p);
                }
            }
        } else if (msg == SSH_MSG_GLOBAL_REQUEST) {
            size_t pos = 1;
            const uint8_t *rname = NULL;
            size_t rlen = 0;
            if (buf_get_str(rx, (size_t)rx_len, &pos, &rname, &rlen) && pos < (size_t)rx_len && rx[pos] != 0) {
                tab->s->payload[0] = SSH_MSG_REQUEST_FAILURE;
                ssh_send_packet(tab->s, tab->s->payload, 1);
            }
        } else if (msg == SSH_MSG_CHANNEL_EOF || msg == SSH_MSG_CHANNEL_CLOSE) {
            tab_disconnect(tab, "Session closed by remote host");
            return true;
        }
    }
    return got_output;
}

void ssh_app_run(void)
{
    ssh_profiles_ensure_loaded();

    ssh_tab_t *tabs = (ssh_tab_t *)kmalloc_psram(sizeof(ssh_tab_t) * SSH_MAX_TABS);
    if (!tabs) tabs = (ssh_tab_t *)kmalloc(sizeof(ssh_tab_t) * SSH_MAX_TABS);
    if (!tabs) {
        kprintf("ssh: out of memory allocating GUI tabs\n");
        return;
    }
    memset(tabs, 0, sizeof(ssh_tab_t) * SSH_MAX_TABS);

    for (int t = 0; t < SSH_MAX_TABS; t++) {
        tabs[t].sock = t;
        strcpy(tabs[t].title, "empty");
        tabs[t].hist = (term_line_t *)kmalloc_psram(sizeof(term_line_t) * SSH_HIST_LINES);
        if (!tabs[t].hist) tabs[t].hist = (term_line_t *)kmalloc(sizeof(term_line_t) * SSH_HIST_LINES);
        tab_reset_term(&tabs[t]);
    }

    int  active_tab     = 0;
    int  sel_profile    = 0;
    bool show_manager   = true;
    bool show_passwords = false;
    bool ctrl_b_prefix  = false;
    bool need_ui_redraw = true;
    int  tilde_state    = 1;

    dq_clear();

    for (;;) {
        /* 0. The on-screen keyboard was closed: whatever was drawn under it meanwhile was discarded, and the
         *    per-row dirty flags would say those rows are up to date.  Repaint everything. */
        if (kconsole_take_refresh()) {
            for (int t = 0; t < SSH_MAX_TABS; t++) tabs[t].all_dirty = true;
            need_ui_redraw = true;
        }

        /* 1. Render UI if needed */
        if (need_ui_redraw) {
            gui_render_bars(tabs, active_tab, show_manager, ctrl_b_prefix);
            if (show_manager) {
                if (sel_profile >= s_profile_count) sel_profile = s_profile_count > 0 ? s_profile_count - 1 : 0;
                if (sel_profile < 0) sel_profile = 0;
                gui_render_manager(tabs, active_tab, sel_profile, show_passwords);
            } else {
                gui_render_terminal(&tabs[active_tab]);
            }
            need_ui_redraw = false;
        } else if (!show_manager) {
            gui_render_terminal(&tabs[active_tab]);
        }

        /* 2. Poll incoming SSH packets across all connected tabs */
        bool rx_changed = false;
        for (int t = 0; t < SSH_MAX_TABS; t++) {
            if (tabs[t].connected) {
                uint32_t wait_ms = (!show_manager && t == active_tab) ? 10u : 0u;
                bool was_conn = tabs[t].connected;
                if (tab_poll_rx(&tabs[t], (!show_manager && t == active_tab), wait_ms)) {
                    rx_changed = true;
                }
                if (was_conn != tabs[t].connected) {
                    need_ui_redraw = true;
                }
            }
        }
        if (rx_changed && !show_manager) {
            gui_render_terminal(&tabs[active_tab]);
        }

        /* 3. Handle keyboard input */
        if (show_manager) {
            int k = dq_pollkey();
            if (k == DQK_NONE) {
                task_sleep(15);
                continue;
            }

            if (k == DQK_UP || k == 'k') {
                if (sel_profile > 0) { sel_profile--; need_ui_redraw = true; }
            } else if (k == DQK_DOWN || k == 'j') {
                if (sel_profile + 1 < s_profile_count) { sel_profile++; need_ui_redraw = true; }
            } else if (k == '1' || k == '2' || k == '3' || k == '4') {
                active_tab = k - '1';
                tabs[active_tab].all_dirty = true;
                need_ui_redraw = true;
            } else if (k == DQK_F1 || k == DQK_F2 || k == DQK_F3 || k == DQK_F4) {
                active_tab = k - DQK_F1;
                tabs[active_tab].all_dirty = true;
                need_ui_redraw = true;
            } else if (k == '\t' || k == DQK_RIGHT || k == 0x14) { /* Tab, Right, Ctrl-T */
                active_tab = (active_tab + 1) % SSH_MAX_TABS;
                tabs[active_tab].all_dirty = true;
                need_ui_redraw = true;
            } else if (k == DQK_LEFT) {
                active_tab = (active_tab + SSH_MAX_TABS - 1) % SSH_MAX_TABS;
                tabs[active_tab].all_dirty = true;
                need_ui_redraw = true;
            } else if (k == ' ' || k == DQK_ESC || k == 0x0F) { /* Space, Esc, Ctrl-O -> Terminal view */
                show_manager = false;
                tabs[active_tab].all_dirty = true;
                need_ui_redraw = true;
            } else if (k == 'p' || k == 'P') {
                show_passwords = !show_passwords;
                need_ui_redraw = true;
            } else if (k == 'w' || k == 'W') {
                wifi_app_run();
                need_ui_redraw = true;
            } else if (k == 'n' || k == 'N' || k == 'a' || k == 'A') {
                char target[96] = "";
                if (dq_input("New SSH Host (e.g. shi3z@tsuginosuke)", target, sizeof(target)) && target[0]) {
                    char pass[64] = "";
                    if (dq_input("Password for Host (saved to /home/ssh_hosts.cfg)", pass, sizeof(pass))) {
                        char u[32] = "";
                        char h[64] = "";
                        uint16_t pt = 22;
                        parse_user_host_port(target, u, sizeof(u), h, sizeof(h), &pt);
                        if (u[0] && h[0]) {
                            ssh_profiles_upsert(u, h, pt, pass);
                            sel_profile = 0;
                        }
                    }
                }
                need_ui_redraw = true;
            } else if ((k == 'e' || k == 'E') && s_profile_count > 0) {
                ssh_profile_t *pr = &s_profiles[sel_profile];
                char target[96];
                if (pr->port != 22) ksnprintf(target, sizeof(target), "%s@%s:%u", pr->user, pr->host, (unsigned)pr->port);
                else                ksnprintf(target, sizeof(target), "%s@%s", pr->user, pr->host);
                char pass[64];
                strncpy(pass, pr->pass, sizeof(pass) - 1);
                pass[sizeof(pass) - 1] = 0;

                if (dq_input("Edit SSH Host (user@host[:port])", target, sizeof(target)) && target[0]) {
                    if (dq_input("Edit Password", pass, sizeof(pass))) {
                        char u[32] = "";
                        char h[64] = "";
                        uint16_t pt = 22;
                        parse_user_host_port(target, u, sizeof(u), h, sizeof(h), &pt);
                        if (u[0] && h[0]) {
                            strncpy(pr->user, u, sizeof(pr->user) - 1);
                            strncpy(pr->host, h, sizeof(pr->host) - 1);
                            pr->port = pt;
                            strncpy(pr->pass, pass, sizeof(pr->pass) - 1);
                            ssh_profiles_save();
                        }
                    }
                }
                need_ui_redraw = true;
            } else if ((k == 'd' || k == 'D' || k == 'x' || k == 'X' || k == DQK_DEL) && s_profile_count > 0) {
                char q[80];
                ksnprintf(q, sizeof(q), "Delete %s@%s?", s_profiles[sel_profile].user, s_profiles[sel_profile].host);
                if (dq_confirm(q)) {
                    ssh_profiles_delete(sel_profile);
                    if (sel_profile >= s_profile_count && sel_profile > 0) sel_profile--;
                }
                need_ui_redraw = true;
            } else if (k == 'k' || k == 'K') {
                if (tabs[active_tab].connected) {
                    tab_disconnect(&tabs[active_tab], "Disconnected from Host Manager");
                    need_ui_redraw = true;
                }
            } else if ((k == DQK_ENTER || k == 't' || k == 'T') && s_profile_count > 0) {
                ssh_profile_t chosen = s_profiles[sel_profile];
                int target_tab = active_tab;

                if (k == 't' || k == 'T') {
                    /* Pick first unconnected tab if available */
                    for (int t = 0; t < SSH_MAX_TABS; t++) {
                        if (!tabs[t].connected) {
                            target_tab = t;
                            break;
                        }
                    }
                } else {
                    /* If this profile is already connected in some tab and current tab is busy, jump or reconnect */
                    for (int t = 0; t < SSH_MAX_TABS; t++) {
                        if (tabs[t].connected &&
                            strcmp(tabs[t].user, chosen.user) == 0 &&
                            strcmp(tabs[t].host, chosen.host) == 0 &&
                            tabs[t].port == chosen.port &&
                            t == active_tab) {
                            /* Already connected in active tab -> just switch to terminal view */
                            show_manager = false;
                            tabs[active_tab].all_dirty = true;
                            need_ui_redraw = true;
                            target_tab = -1;
                            break;
                        }
                    }
                }

                if (target_tab >= 0) {
                    active_tab = target_tab;
                    if (tab_connect_profile(&tabs[active_tab], chosen.user, chosen.host, chosen.port, chosen.pass)) {
                        sel_profile = 0; /* Upsert moved it to top */
                        show_manager = false;
                        tabs[active_tab].all_dirty = true;
                    }
                    need_ui_redraw = true;
                }
            } else if (k == 'q' || k == 'Q') {
                bool any_conn = false;
                for (int t = 0; t < SSH_MAX_TABS; t++) {
                    if (tabs[t].connected) any_conn = true;
                }
                if (!any_conn || dq_confirm("Close all SSH tabs and quit?")) {
                    break;
                }
                need_ui_redraw = true;
            }
            continue;
        }

        /* 4. Terminal Mode Input Handling */
        ssh_tab_t *act = &tabs[active_tab];

        /* 4a. If scrolled up in history (`scroll_offset > 0`), handle scroll navigation keys */
        if (act->scroll_offset > 0) {
            int k = dq_pollkey();
            if (k == DQK_NONE) {
                task_sleep(10);
                continue;
            }
            int prev_off = act->scroll_offset;
            if (k == DQK_UP || k == 'k') {
                if (act->scroll_offset < act->hist_count) act->scroll_offset++;
            } else if (k == DQK_DOWN || k == 'j') {
                if (act->scroll_offset > 0) act->scroll_offset--;
            } else if (k == DQK_PGUP || k == 0x15 || k == 'u' || k == 'b') {
                act->scroll_offset += 15;
                if (act->scroll_offset > act->hist_count) act->scroll_offset = act->hist_count;
            } else if (k == DQK_PGDN || k == 0x04 || k == 'd' || k == ' ') {
                act->scroll_offset -= 15;
                if (act->scroll_offset < 0) act->scroll_offset = 0;
            } else if (k == DQK_HOME || k == 'g') {
                act->scroll_offset = act->hist_count;
            } else if (k == DQK_END || k == 'G' || k == DQK_ENTER || k == DQK_ESC || k == 'q') {
                act->scroll_offset = 0;
            } else if (k == 0x0F) { /* Ctrl-O */
                show_manager = true;
                need_ui_redraw = true;
                continue;
            } else if (k == 0x14) { /* Ctrl-T */
                active_tab = (active_tab + 1) % SSH_MAX_TABS;
                tabs[active_tab].all_dirty = true;
                need_ui_redraw = true;
                continue;
            }
            if (act->scroll_offset != prev_off) {
                act->all_dirty = true;
                need_ui_redraw = true;
            }
            continue;
        }

        /* 4b. Live Terminal Mode: read raw bytes from console and intercept local hotkeys */
        int c = kconsole_getc();
        if (c < 0) {
            if (!act->connected) task_sleep(15);
            continue;
        }

        if (ctrl_b_prefix) {
            ctrl_b_prefix = false;
            need_ui_redraw = true;
            if (c >= '1' && c <= '4') {
                active_tab = c - '1';
                tabs[active_tab].all_dirty = true;
                continue;
            }
            if (c == 'n' || c == 'N' || c == '\t') {
                active_tab = (active_tab + 1) % SSH_MAX_TABS;
                tabs[active_tab].all_dirty = true;
                continue;
            }
            if (c == 'p' || c == 'P') {
                active_tab = (active_tab + SSH_MAX_TABS - 1) % SSH_MAX_TABS;
                tabs[active_tab].all_dirty = true;
                continue;
            }
            if (c == 'o' || c == 'O' || c == 'c' || c == 'C' || c == 'g' || c == 'G') {
                show_manager = true;
                continue;
            }
            if (c == 'u' || c == 'U' || c == '[') {
                if (act->hist_count > 0) {
                    act->scroll_offset = (act->hist_count >= 10) ? 10 : act->hist_count;
                    act->all_dirty = true;
                }
                continue;
            }
            if (c == 'x' || c == 'X') {
                tab_disconnect(act, "Disconnected by ^\\ x");
                show_manager = true;
                continue;
            }
            if (c == 'q' || c == 'Q' || c == 'd' || c == 'D') {
                show_manager = true;
                continue;
            }
            if (c == 0x1C) { /* Ctrl-\ Ctrl-\ sends literal Ctrl-\ */
                uint8_t b = 0x1C;
                tab_send_bytes(act, &b, 1);
                continue;
            }
            continue;
        }

        /* Global hotkeys in live terminal mode (Note: ^B is NOT intercepted so remote tmux works) */
        if (c == 0x0F) { /* Ctrl-O: Open Host Manager GUI */
            show_manager = true;
            need_ui_redraw = true;
            continue;
        }
        if (c == 0x14) { /* Ctrl-T: Next Tab */
            active_tab = (active_tab + 1) % SSH_MAX_TABS;
            tabs[active_tab].all_dirty = true;
            need_ui_redraw = true;
            continue;
        }
        if (c == 0x1C) { /* Ctrl-\: Local SSH GUI prefix key */
            ctrl_b_prefix = true;
            need_ui_redraw = true;
            continue;
        }
        if (c == 0x15) { /* Ctrl-U: Scroll up history */
            if (act->hist_count > 0) {
                act->scroll_offset = (act->hist_count >= 15) ? 15 : act->hist_count;
                act->all_dirty = true;
                need_ui_redraw = true;
            }
            continue;
        }

        /* If the current tab is disconnected, Enter/Space/Esc opens the Host Manager GUI */
        if (!act->connected) {
            if (c == '\r' || c == '\n' || c == ' ' || c == 0x1B || c == 'q') {
                show_manager = true;
                need_ui_redraw = true;
            } else if (c >= '1' && c <= '4') {
                active_tab = c - '1';
                tabs[active_tab].all_dirty = true;
                need_ui_redraw = true;
            }
            continue;
        }

        /* Check for escape sequences that correspond to local F1..F4 or PageUp/PageDown */
        if (c == 0x1B) {
            int c2 = kconsole_getc();
            if (c2 < 0) {
                task_sleep(15);
                c2 = kconsole_getc();
            }
            if (c2 < 0) {
                uint8_t esc = 0x1B;
                tab_send_bytes(act, &esc, 1);
                continue;
            }
            int c3 = kconsole_getc();
            if (c3 < 0) {
                task_sleep(10);
                c3 = kconsole_getc();
            }
            if (c2 == 'O' && c3 >= 'P' && c3 <= 'S') {
                /* F1..F4 -> Switch to Tab 1..4 */
                active_tab = c3 - 'P';
                tabs[active_tab].all_dirty = true;
                need_ui_redraw = true;
                continue;
            }
            if (c2 == '[' && (c3 == '5' || c3 == '6')) {
                int c4 = kconsole_getc();
                if (c4 < 0) {
                    task_sleep(10);
                    c4 = kconsole_getc();
                }
                if (c4 == '~' && c3 == '5') {
                    /* PageUp -> Scroll up history */
                    if (act->hist_count > 0) {
                        act->scroll_offset = (act->hist_count >= 15) ? 15 : act->hist_count;
                        act->all_dirty = true;
                        need_ui_redraw = true;
                    }
                    continue;
                }
                /* Forward sequence if not PageUp */
                uint8_t seq[4] = { 0x1B, (uint8_t)c2, (uint8_t)c3, (uint8_t)(c4 > 0 ? c4 : '~') };
                tab_send_bytes(act, seq, c4 > 0 ? 4 : 3);
                continue;
            }
            uint8_t seq[3] = { 0x1B, (uint8_t)c2, (uint8_t)(c3 > 0 ? c3 : 0) };
            tab_send_bytes(act, seq, c3 > 0 ? 3 : 2);
            continue;
        }

        /* Support OpenSSH `~.` disconnect sequence after newline */
        if (tilde_state == 1 && c == '~') {
            tilde_state = 2;
            continue;
        }
        if (tilde_state == 2) {
            if (c == '.') {
                tab_disconnect(act, "Disconnected by ~.");
                show_manager = true;
                need_ui_redraw = true;
                tilde_state = 1;
                continue;
            }
            uint8_t tilde = '~';
            tab_send_bytes(act, &tilde, 1);
            tilde_state = 0;
        }
        if (c == '\r' || c == '\n') tilde_state = 1;
        else tilde_state = 0;

        uint8_t out_ch = (uint8_t)c;
        tab_send_bytes(act, &out_ch, 1);
    }

    /* Cleanup all tabs and free PSRAM buffers */
    for (int t = 0; t < SSH_MAX_TABS; t++) {
        tab_disconnect(&tabs[t], NULL);
        if (tabs[t].hist) {
            kfree(tabs[t].hist);
            tabs[t].hist = NULL;
        }
    }
    kfree(tabs);
    dq_clear();
}

int ssh_cmd_main(int argc, char **argv)
{
    ssh_profiles_ensure_loaded();

    if (argc < 2 || strcmp(argv[1], "gui") == 0) {
        fbcon_pause(true);
        task_sleep(80);
        ssh_app_run();
        fbcon_pause(false);
        return 0;
    }

    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "help") == 0) {
        kprintf("usage: ssh                                    (open Multi-Tab Graphical SSH Client)\n"
                "       ssh [user@]host[:port] [-p port] [-l user] [-pw password] [command...]\n"
                "       ssh key [show]                         (show saved ssh-ed25519 key & public key)\n"
                "       ssh key set <64-hex-seed-or-b64>       (save Ed25519 private key to Flash & /home)\n"
                "       ssh key import [file]                  (import OpenSSH id_ed25519 file or paste)\n"
                "       ssh key gen                            (generate new Ed25519 keypair in Flash)\n"
                "       ssh key clear                          (delete saved Ed25519 private key)\n"
                "       ssh hosts                              (list saved hosts & passwords)\n"
                "       ssh add user@host[:port] [password]    (save a host & password)\n"
                "       ssh del <number>                       (delete saved host #)\n");
        return 0;
    }

    if (strcmp(argv[1], "key") == 0 || strcmp(argv[1], "keys") == 0) {
        const char *sub = (argc >= 3) ? argv[2] : "show";
        if (strcmp(sub, "show") == 0 || strcmp(sub, "status") == 0) {
            uint8_t seed[32], pub[32];
            bool from_flash = false;
            if (!ssh_load_ed25519_key(seed, pub, &from_flash)) {
                kprintf("ssh: no Ed25519 private key saved yet.\n"
                        "     Use 'ssh key set <64-hex-seed>', 'ssh key import', or 'ssh key gen'.\n");
                return 1;
            }
            memset(seed, 0, sizeof(seed));
            char pub_str[128];
            ssh_format_pubkey_openssh(pub, pub_str, sizeof(pub_str));
            kprintf("Saved Ed25519 SSH key (%s, Flash @ 0x%06x):\n  %s\n",
                    SSH_KEY_PATH, (unsigned)SSH_FLASH_KEY_ADDR, pub_str);
            return 0;
        }
        if (strcmp(sub, "set") == 0 && argc >= 4) {
            uint8_t seed[32], pub[32];
            if (!ssh_parse_ed25519_seed((const uint8_t *)argv[3], strlen(argv[3]), seed, pub)) {
                kprintf("ssh key set: invalid Ed25519 seed (expected 64 hex chars or OpenSSH base64)\n");
                return 1;
            }
            bool flash_ok = ssh_save_ed25519_key(seed, pub);
            memset(seed, 0, sizeof(seed));
            char pub_str[128];
            ssh_format_pubkey_openssh(pub, pub_str, sizeof(pub_str));
            kprintf("ssh: saved Ed25519 private key to %s%s\nPublic key:\n  %s\n",
                    SSH_KEY_PATH,
                    flash_ok ? " and SPI Flash (0x3F0000, persistent)" : "",
                    pub_str);
            return 0;
        }
        if (strcmp(sub, "import") == 0) {
            static uint8_t ibuf[1024];
            size_t ilen = 0;
            if (argc >= 4) {
                int fd = vfs_open(argv[3], VFS_O_READ);
                if (fd < 0) {
                    kprintf("ssh key import: cannot open '%s'\n", argv[3]);
                    return 1;
                }
                int r = vfs_read(fd, ibuf, sizeof(ibuf) - 1);
                vfs_close(fd);
                if (r > 0) ilen = (size_t)r;
            } else {
                kprintf("Paste OpenSSH Ed25519 private key (-----BEGIN OPENSSH PRIVATE KEY-----) or 64-hex seed:\n");
                while (ilen < sizeof(ibuf) - 1) {
                    int c = kconsole_getc();
                    if (c < 0) { task_sleep(10); continue; }
                    if (c == 0x03) { kprintf("\nCancelled.\n"); return 1; }
                    ibuf[ilen++] = (uint8_t)c;
                    ibuf[ilen] = 0;
                    if (c == '\n' || c == '\r') {
                        if (strstr((const char *)ibuf, "-----END OPENSSH PRIVATE KEY-----") != NULL) break;
                        if (strstr((const char *)ibuf, "-----BEGIN") == NULL && ilen >= 64) break;
                    }
                }
            }
            ibuf[ilen] = 0;
            uint8_t seed[32], pub[32];
            bool ok = ssh_parse_ed25519_seed(ibuf, ilen, seed, pub);
            memset(ibuf, 0, sizeof(ibuf));
            if (!ok) {
                kprintf("ssh key import: failed to parse Ed25519 private key\n");
                return 1;
            }
            bool flash_ok = ssh_save_ed25519_key(seed, pub);
            memset(seed, 0, sizeof(seed));
            char pub_str[128];
            ssh_format_pubkey_openssh(pub, pub_str, sizeof(pub_str));
            kprintf("ssh: imported Ed25519 private key to %s%s\nPublic key:\n  %s\n",
                    SSH_KEY_PATH,
                    flash_ok ? " and SPI Flash (0x3F0000, persistent)" : "",
                    pub_str);
            return 0;
        }
        if (strcmp(sub, "gen") == 0) {
            uint8_t seed[32], pub[32];
            ssh_random_bytes(seed, 32);
            bool flash_ok = ssh_save_ed25519_key(seed, pub);
            memset(seed, 0, sizeof(seed));
            char pub_str[128];
            ssh_format_pubkey_openssh(pub, pub_str, sizeof(pub_str));
            kprintf("ssh: generated new Ed25519 keypair in %s%s\nAdd this line to ~/.ssh/authorized_keys:\n  %s\n",
                    SSH_KEY_PATH,
                    flash_ok ? " and SPI Flash (0x3F0000, persistent)" : "",
                    pub_str);
            return 0;
        }
        if (strcmp(sub, "clear") == 0 || strcmp(sub, "del") == 0) {
            ssh_clear_ed25519_key();
            kprintf("ssh: cleared saved Ed25519 private key from Flash and %s\n", SSH_KEY_PATH);
            return 0;
        }
        kprintf("usage: ssh key [show | set <64-hex-seed> | import [file] | gen | clear]\n");
        return 1;
    }

    if (strcmp(argv[1], "hosts") == 0 || strcmp(argv[1], "list") == 0) {
        kprintf("Saved SSH profiles (%s):\n", SSH_HOSTS_CFG_PATH);
        for (int i = 0; i < s_profile_count; i++) {
            kprintf("  %2d: %s@%s:%u (password: %s)\n",
                    i + 1,
                    s_profiles[i].user,
                    s_profiles[i].host,
                    (unsigned)s_profiles[i].port,
                    s_profiles[i].pass[0] ? "saved" : "ssh-key");
        }
        return 0;
    }

    if (strcmp(argv[1], "add") == 0 && argc >= 3) {
        char u[32] = "";
        char h[64] = "";
        uint16_t pt = 22;
        parse_user_host_port(argv[2], u, sizeof(u), h, sizeof(h), &pt);
        const char *pw = (argc >= 4) ? argv[3] : "";
        if (!u[0] || !h[0]) {
            kprintf("usage: ssh add user@host[:port] [password]\n");
            return 1;
        }
        ssh_profiles_upsert(u, h, pt, pw);
        kprintf("ssh: saved %s@%s:%u to %s\n", u, h, (unsigned)pt, SSH_HOSTS_CFG_PATH);
        return 0;
    }

    if (strcmp(argv[1], "del") == 0 && argc >= 3) {
        int idx = 0;
        for (const char *s = argv[2]; *s >= '0' && *s <= '9'; s++) idx = idx * 10 + (*s - '0');
        if (idx < 1 || idx > s_profile_count) {
            kprintf("ssh: invalid profile number %d\n", idx);
            return 1;
        }
        kprintf("ssh: deleted %s@%s\n", s_profiles[idx - 1].user, s_profiles[idx - 1].host);
        ssh_profiles_delete(idx - 1);
        return 0;
    }

    char user[48] = "";
    char host[64] = "";
    char pass[64] = "";
    char cmd[256] = "";
    uint16_t port = 22;

    int i = 1;
    while (i < argc) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            uint32_t p = 0;
            for (const char *s = argv[i + 1]; *s >= '0' && *s <= '9'; s++) p = p * 10u + (uint32_t)(*s - '0');
            if (p > 0 && p <= 65535u) port = (uint16_t)p;
            i += 2;
        } else if (strcmp(argv[i], "-l") == 0 && i + 1 < argc) {
            strncpy(user, argv[i + 1], sizeof(user) - 1);
            i += 2;
        } else if ((strcmp(argv[i], "-pw") == 0 || strcmp(argv[i], "--pass") == 0) && i + 1 < argc) {
            strncpy(pass, argv[i + 1], sizeof(pass) - 1);
            i += 2;
        } else if (!host[0] && argv[i][0] != '-') {
            parse_user_host_port(argv[i], user, sizeof(user), host, sizeof(host), &port);
            i++;
        } else {
            size_t clen = strlen(cmd);
            if (clen > 0 && clen + 1 < sizeof(cmd)) {
                cmd[clen++] = ' ';
                cmd[clen] = 0;
            }
            strncat(cmd, argv[i], sizeof(cmd) - clen - 1);
            i++;
        }
    }

    if (!host[0]) {
        kprintf("usage: ssh [user@]host[:port] [-p port] [-l user] [-pw password] [command...]\n");
        return 1;
    }

    return ssh_connect_and_run(user, host, port, pass, sizeof(pass), cmd[0] ? cmd : NULL);
}

void ssh_main(void *arg)
{
    (void)arg;
    fbcon_pause(true);
    task_sleep(80);
    ssh_app_run();
    fbcon_pause(false);
}

