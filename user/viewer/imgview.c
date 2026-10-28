#include "imgview.h"
#include "../tui/dqwin.h"
#include <m5os/display.h>
#include <m5os/vfs.h>
#include <m5os/kmalloc.h>
#include <m5os/errno.h>
#include <m5os/kprintf.h>

#define MAX_FILE   (8u * 1024u * 1024u)
#define MAX_ZOOM   4                      /* small images are enlarged by whole factors, up to this */
#define LINE_BYTES 4                      /* BMP rows are padded to a multiple of 4 bytes */

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* A decoded source image as a way to ask "what colour is pixel (x, y)": the formats differ, the scaling does not. */
typedef struct {
    const uint8_t *data, *pal;           /* pixel data; BMP palette (4 bytes per entry, B G R x) */
    int w, h, bpp, stride;
    bool top_down, gray;
    uint32_t mask_r, mask_g, mask_b;     /* 16/32-bit BMP channel masks */
} src_t;

static uint16_t rgb565(int r, int g, int b) { return RGB565(r, g, b); }

static uint32_t take(uint32_t v, uint32_t mask)             /* extract a bit field and scale it to 8 bits */
{
    if (!mask) return 0;
    int shift = 0;
    while (!((mask >> shift) & 1)) shift++;
    uint32_t m = mask >> shift, f = (v & mask) >> shift;
    int bits = 0;
    while ((m >> bits) & 1) bits++;
    return bits >= 8 ? f >> (bits - 8) : (f * 255u) / m;
}

static uint16_t pixel(const src_t *s, int x, int y)
{
    int row = s->top_down ? y : s->h - 1 - y;
    const uint8_t *p = s->data + (size_t)row * (size_t)s->stride;
    switch (s->bpp) {
    case 24: p += x * 3; return rgb565(p[2], p[1], p[0]);                          /* BMP stores B G R */
    case 32: { p += x * 4; uint32_t v = le32(p);
               return s->mask_r ? rgb565((int)take(v, s->mask_r), (int)take(v, s->mask_g), (int)take(v, s->mask_b)) : rgb565(p[2], p[1], p[0]); }
    case 16: { uint32_t v = le16(p + x * 2);
               return rgb565((int)take(v, s->mask_r), (int)take(v, s->mask_g), (int)take(v, s->mask_b)); }
    case 8:  if (s->pal) { const uint8_t *e = s->pal + 4 * p[x]; return rgb565(e[2], e[1], e[0]); }
             return rgb565(p[x], p[x], p[x]);
    case 83: p += x * 3; return rgb565(p[0], p[1], p[2]);                          /* PPM P6: R G B (internal code) */
    default: return 0;
    }
}

static bool parse_bmp(const uint8_t *f, size_t n, src_t *s)
{
    if (n < 54 || f[0] != 'B' || f[1] != 'M') return false;
    uint32_t off = le32(f + 10), dib = le32(f + 14);
    if (dib < 40 || off >= n) return false;
    int32_t w = (int32_t)le32(f + 18), h = (int32_t)le32(f + 22);
    int bpp = le16(f + 28);
    uint32_t comp = le32(f + 30), colors = le32(f + 46);
    if (w <= 0 || h == 0 || (comp != 0 && comp != 3)) return false;
    s->top_down = h < 0;
    s->w = w; s->h = h < 0 ? -h : h; s->bpp = bpp;
    if (bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) return false;
    s->stride = ((w * bpp + 31) / 32) * LINE_BYTES;
    s->data = f + off;
    if ((size_t)off + (size_t)s->stride * (size_t)s->h > n) return false;
    s->pal = NULL; s->mask_r = s->mask_g = s->mask_b = 0;
    if (bpp == 8) { if (!colors) colors = 256; s->pal = f + 14 + dib; if ((size_t)(s->pal - f) + colors * 4 > n) return false; }
    if (bpp == 16 || bpp == 32) {
        if (comp == 3 && dib >= 52) { s->mask_r = le32(f + 54); s->mask_g = le32(f + 58); s->mask_b = le32(f + 62); }
        else if (bpp == 16) { s->mask_r = 0x7C00; s->mask_g = 0x03E0; s->mask_b = 0x001F; }       /* default 5-5-5 */
    }
    return true;
}

static const uint8_t *ppm_int(const uint8_t *p, const uint8_t *end, int *out)
{
    while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t' || *p == '#')) {
        if (*p == '#') while (p < end && *p != '\n') p++; else p++;                   /* comments run to end of line */
    }
    int v = 0;
    if (p >= end || *p < '0' || *p > '9') return NULL;
    while (p < end && *p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
    *out = v;
    return p;
}

static bool parse_ppm(const uint8_t *f, size_t n, src_t *s)
{
    if (n < 10 || f[0] != 'P' || (f[1] != '6' && f[1] != '5')) return false;
    bool color = f[1] == '6';
    const uint8_t *p = f + 2, *end = f + n;
    int w, h, maxv;
    if (!(p = ppm_int(p, end, &w)) || !(p = ppm_int(p, end, &h)) || !(p = ppm_int(p, end, &maxv))) return false;
    if (w <= 0 || h <= 0 || maxv != 255 || p >= end) return false;
    p++;                                                  /* exactly one whitespace byte before the data */
    s->w = w; s->h = h; s->top_down = true; s->pal = NULL; s->data = p;
    s->bpp = color ? 83 : 8; s->stride = color ? w * 3 : w;
    s->mask_r = s->mask_g = s->mask_b = 0;
    return (size_t)(p - f) + (size_t)s->stride * (size_t)h <= n;
}

static const char *s_why = "";
const char *imgview_last_error(void) { return s_why; }

uint16_t *imgview_decode(const char *path, int max_w, int max_h, int *ow, int *oh)
{
    vfs_info_t st;
    s_why = "";
    int rc = vfs_stat(path, &st);
    if (rc != ERR_OK)                { s_why = "cannot stat the file"; return NULL; }
    if (st.type != FS_TYPE_FILE)     { s_why = "not a regular file"; return NULL; }
    if (st.size < 16)                { s_why = "file too small"; return NULL; }
    if (st.size > MAX_FILE)          { s_why = "file larger than 8 MB"; return NULL; }
    uint8_t *file = kmalloc_psram(st.size);
    if (!file)                       { s_why = "out of PSRAM"; return NULL; }
    int fd = vfs_open(path, VFS_O_READ);
    size_t got = 0;
    if (fd >= 0) {
        int r;
        while (got < st.size && (r = vfs_read(fd, file + got, st.size - got)) > 0) got += (size_t)r;
        vfs_close(fd);
    }
    src_t s;
    uint16_t *out = NULL;
    if (got != st.size) s_why = "short read";
    else if (!parse_bmp(file, got, &s) && !parse_ppm(file, got, &s)) s_why = "not a supported BMP/PPM";
    else {
        /* Fit: enlarge small images by a whole factor, shrink big ones to the screen (nearest neighbour). */
        int zoom = 1;
        while (zoom < MAX_ZOOM && s.w * (zoom + 1) <= max_w && s.h * (zoom + 1) <= max_h) zoom++;
        int w = s.w * zoom, h = s.h * zoom;
        if (w > max_w) { h = (int)((int64_t)s.h * max_w / s.w); w = max_w; }
        if (h > max_h) { w = (int)((int64_t)s.w * max_h / s.h); h = max_h; }
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        out = kmalloc_psram((size_t)w * (size_t)h * 2);
        if (!out) s_why = "out of PSRAM for the picture";
        else {
            for (int y = 0; y < h; y++) {
                int sy = (int)((int64_t)y * s.h / h);
                for (int x = 0; x < w; x++) out[(size_t)y * (size_t)w + (size_t)x] = pixel(&s, (int)((int64_t)x * s.w / w), sy);
            }
            *ow = w; *oh = h;
        }
    }
    kfree(file);
    return out;
}

bool imgview_show(const char *path)
{
    int w = 0, h = 0;
    uint16_t *px = imgview_decode(path, DISPLAY_WIDTH, DISPLAY_HEIGHT, &w, &h);
    if (!px) { dq_message("Cannot show this file:", imgview_last_error()); return false; }
    dq_clear();
    display_draw_bitmap((DISPLAY_WIDTH - w) / 2, (DISPLAY_HEIGHT - h) / 2, w, h, px);
    char info[100];
    ksnprintf(info, sizeof info, " %dx%d  Q:close ", w, h);
    dq_text(0, dq_rows() - 1, info);
    for (;;) {
        int k = dq_getkey();
        if (k == 'q' || k == 'Q' || k == DQK_ESC || k == DQK_ENTER) break;
    }
    kfree(px);
    return true;
}

int imgview_write_bmp(const char *path, const uint16_t *px, int w, int h)
{
    int stride = ((w * 24 + 31) / 32) * LINE_BYTES;
    uint32_t data = (uint32_t)stride * (uint32_t)h, total = 54 + data;
    uint8_t hdr[54] = { 'B', 'M' };
    hdr[2] = (uint8_t)total; hdr[3] = (uint8_t)(total >> 8); hdr[4] = (uint8_t)(total >> 16); hdr[5] = (uint8_t)(total >> 24);
    hdr[10] = 54; hdr[14] = 40;
    hdr[18] = (uint8_t)w; hdr[19] = (uint8_t)(w >> 8); hdr[20] = (uint8_t)(w >> 16);
    hdr[22] = (uint8_t)h; hdr[23] = (uint8_t)(h >> 8); hdr[24] = (uint8_t)(h >> 16);
    hdr[26] = 1; hdr[28] = 24;
    hdr[34] = (uint8_t)data; hdr[35] = (uint8_t)(data >> 8); hdr[36] = (uint8_t)(data >> 16); hdr[37] = (uint8_t)(data >> 24);
    int fd = vfs_open(path, VFS_O_WRITE | VFS_O_CREATE | VFS_O_TRUNC);
    if (fd < 0) return fd;
    int rc = vfs_write(fd, hdr, sizeof hdr) == (int)sizeof hdr ? 0 : ERR_NOSPC;
    uint8_t *row = kmalloc((size_t)stride);
    if (!row) { vfs_close(fd); return ERR_NOMEM; }
    for (int y = h - 1; y >= 0 && rc == 0; y--) {                    /* bottom-up, as BMP wants */
        for (int x = 0; x < w; x++) {
            uint16_t c = px[(size_t)y * (size_t)w + (size_t)x];
            row[x * 3 + 0] = (uint8_t)((c & 0x1F) << 3);              /* B */
            row[x * 3 + 1] = (uint8_t)(((c >> 5) & 0x3F) << 2);       /* G */
            row[x * 3 + 2] = (uint8_t)(((c >> 11) & 0x1F) << 3);      /* R */
        }
        for (int i = w * 3; i < stride; i++) row[i] = 0;
        if (vfs_write(fd, row, (size_t)stride) != stride) rc = ERR_NOSPC;
    }
    kfree(row);
    vfs_close(fd);
    if (rc) vfs_unlink(path);
    return rc;
}
