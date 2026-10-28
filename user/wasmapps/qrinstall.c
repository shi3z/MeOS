/*
 * Installing an app from a QR code: scan with the camera, download over HTTPS, check the hash if the code carries one,
 * show what the app wants to be allowed to do, install on a yes.
 *
 * The code holds either   meos://install?url=<percent-encoded https URL>[&sha256=<64 hex digits>]
 * or just a plain          https://.../app.wasm
 * With sha256 the download is pinned to exactly the file the code's author meant; without it the URL's server decides.
 * Either way the app is sandboxed (kernel/include/m5os/wasm.h): the only thing it can ever do is what the dialog says.
 */
#include "qrinstall.h"
#include "../tui/dqwin.h"
#include "../web/web_app.h"
#include "../../third_party/quirc/quirc.h"
#include <m5os/camera.h>
#include <m5os/display.h>
#include <m5os/wasm.h>
#include <m5os/app.h>
#include <m5os/sha256.h>
#include <m5os/errno.h>
#include <m5os/kmalloc.h>
#include <m5os/kprintf.h>
#include <m5os/task.h>
#include <m5os/hal_time.h>
#include <string.h>

#define PREVIEW_W   640
#define PREVIEW_H   360
#define DL_MAX      (576u * 1024u)

/* ---- QR decoding ---------------------------------------------------------------------------- */

static struct quirc *s_small, *s_full;

static inline uint8_t luma(uint16_t p)
{
    unsigned r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
    return (uint8_t)((r * 77u * 8u + g * 150u * 4u + b * 29u * 8u) >> 8);
}

static bool decode_with(struct quirc *q, char *out, size_t cap)
{
    quirc_end(q);
    int n = quirc_count(q);
    for (int i = 0; i < n; i++) {
        struct quirc_code code;
        struct quirc_data data;
        quirc_extract(q, i, &code);
        quirc_decode_error_t err = quirc_decode(&code, &data);
        if (err == QUIRC_ERROR_DATA_ECC) { quirc_flip(&code); err = quirc_decode(&code, &data); }   /* mirrored picture */
        if (err == QUIRC_SUCCESS && (size_t)data.payload_len < cap) {
            memcpy(out, data.payload, (size_t)data.payload_len);
            out[data.payload_len] = 0;
            return true;
        }
    }
    return false;
}

static bool try_decode(const uint16_t *frame, bool full, char *out, size_t cap)
{
    struct quirc *q = full ? s_full : s_small;
    int w, h;
    uint8_t *img = quirc_begin(q, &w, &h);
    if (full) {
        for (size_t i = 0; i < (size_t)w * h; i++) img[i] = luma(frame[i]);
    } else {
        for (int y = 0; y < h; y++) {
            const uint16_t *r0 = &frame[(size_t)(y * 2) * CAMERA_WIDTH], *r1 = r0 + CAMERA_WIDTH;
            for (int x = 0; x < w; x++)
                img[(size_t)y * w + x] = (uint8_t)(((unsigned)luma(r0[2 * x]) + luma(r0[2 * x + 1]) + luma(r1[2 * x]) + luma(r1[2 * x + 1])) >> 2);
        }
    }
    return decode_with(q, out, cap);
}

static void padded(int cx, int cy, const char *s)
{
    char line[200];
    int n = 0, w = dq_cols() - cx - 2;
    while (s[n] && n < w && n < (int)sizeof line - 1) { line[n] = s[n]; n++; }
    while (n < w && n < (int)sizeof line - 1) line[n++] = ' ';
    line[n] = 0;
    dq_text(cx, cy, line);
}

/* Shows the camera and waits for a QR code.  False on Q / Esc or camera trouble. */
static bool scan(char *out, size_t cap)
{
    if (!camera_init() || !camera_start()) { dq_message("Camera hardware init failed", "Check the camera connection."); return false; }
    if (!s_small) s_small = quirc_new();
    if (!s_full)  s_full = quirc_new();
    uint16_t *preview = kmalloc_psram((size_t)PREVIEW_W * PREVIEW_H * 2u);
    if (!s_small || !s_full || !preview || quirc_resize(s_small, PREVIEW_W, PREVIEW_H) < 0 || quirc_resize(s_full, CAMERA_WIDTH, CAMERA_HEIGHT) < 0) {
        kfree(preview);
        camera_stop();
        dq_message("Out of memory for the QR reader.", NULL);
        return false;
    }
    int cols = dq_cols(), rows = dq_rows(), fy = rows - 3;
    dq_clear();
    dq_window(0, 0, cols, 3);
    dq_text(2, 1, "INSTALL AN APP  -  show its QR code to the camera");
    dq_window(0, 3, cols, fy - 3);
    dq_window(0, fy, cols, 3);
    dq_text(2, fy + 1, "^C / Q / Esc: cancel");

    bool found = false;
    uint32_t last_seq = 0, frames = 0;
    int vx = (DISPLAY_WIDTH - PREVIEW_W) / 2, vy = 3 * 16 + ((fy - 3) * 16 - PREVIEW_H) / 2;
    while (!found) {
        uint32_t seq = 0;
        const uint16_t *frame = camera_frame(&seq);
        if (frame && seq != last_seq) {
            last_seq = seq;
            frames++;
            if ((seq & 3u) == 0) camera_auto_exposure_step(frame);
            camera_status_t st;
            camera_get_status(&st);
            for (int y = 0; y < PREVIEW_H; y++) {
                const uint16_t *src = &frame[(size_t)(y * 2) * CAMERA_WIDTH];
                uint16_t *dst = &preview[(size_t)y * PREVIEW_W];
                if (st.mirror) for (int x = 0; x < PREVIEW_W; x++) dst[x] = src[CAMERA_WIDTH - 1 - 2 * x];
                else           for (int x = 0; x < PREVIEW_W; x++) dst[x] = src[2 * x];
            }
            display_draw_bitmap(vx, vy, PREVIEW_W, PREVIEW_H, preview);
            if (frames % 2 == 0) found = try_decode(frame, (frames / 2) % 3 == 0, out, cap);       /* mostly the quick small pass */
        } else {
            task_sleep(5);
        }
        int k = dq_pollkey();
        if (k == 0x03 || k == 'q' || k == 'Q' || k == DQK_ESC) break;
    }
    camera_stop();
    kfree(preview);
    return found;
}

/* ---- the code's content ----------------------------------------------------------------------- */

static int hexval(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

/* Finds "key=" in the query part and copies its percent-decoded value. */
static bool query_value(const char *q, const char *key, char *out, size_t cap)
{
    size_t kl = strlen(key);
    for (const char *p = q; *p;) {
        if (strncmp(p, key, kl) == 0 && p[kl] == '=') {
            p += kl + 1;
            size_t n = 0;
            while (*p && *p != '&' && n + 1 < cap) {
                if (*p == '%' && hexval(p[1]) >= 0 && hexval(p[2]) >= 0) { out[n++] = (char)(hexval(p[1]) * 16 + hexval(p[2])); p += 3; }
                else out[n++] = *p++;
            }
            out[n] = 0;
            return true;
        }
        while (*p && *p != '&') p++;
        if (*p == '&') p++;
    }
    return false;
}

static bool parse_code(const char *code, char *url, size_t url_cap, uint8_t want_hash[32], bool *has_hash)
{
    *has_hash = false;
    if (strncmp(code, "meos://install?", 15) == 0) {
        const char *q = code + 15;
        if (!query_value(q, "url", url, url_cap)) return false;
        char hex[80];
        if (query_value(q, "sha256", hex, sizeof hex)) {
            if (strlen(hex) != 64) return false;
            for (int i = 0; i < 32; i++) {
                int a = hexval(hex[2 * i]), b = hexval(hex[2 * i + 1]);
                if (a < 0 || b < 0) return false;
                want_hash[i] = (uint8_t)(a * 16 + b);
            }
            *has_hash = true;
        }
    } else if (strncmp(code, "https://", 8) == 0 || strncmp(code, "http://", 7) == 0) {
        if (strlen(code) >= url_cap) return false;
        strcpy(url, code);
    } else {
        return false;
    }
    return strncmp(url, "https://", 8) == 0 || strncmp(url, "http://", 7) == 0;
}

/* ---- confirmation ----------------------------------------------------------------------------- */

static const struct { uint32_t bit; const char *text; } PERMS[] = {
    { APP_PERM_CONSOLE,  "show text and read the keyboard" },
    { APP_PERM_DISPLAY,  "take over the whole screen (and keep the keyboard)" },
    { APP_PERM_INPUT,    "read the buttons" },
    { APP_PERM_FS_READ,  "read your files" },
    { APP_PERM_FS_WRITE, "create, change and delete your files" },
    { APP_PERM_SPAWN,    "start background tasks" },
    { APP_PERM_IPC,      "talk to other apps" },
};

static bool confirm(const wasm_manifest_t *m, size_t size, const char *url, bool pinned, bool replaces)
{
    int w = dq_cols() * 4 / 5, h = 14, x = (dq_cols() - w) / 2, y = (dq_rows() - h) / 2;
    dq_clear();
    dq_window(x, y, w, h);
    char line[160];
    ksnprintf(line, sizeof line, "Install \"%s\" %s ?", m->name, m->version);
    dq_text(x + 2, y + 1, line);
    ksnprintf(line, sizeof line, "%u KB, may use %u KB of memory%s", (unsigned)((size + 1023) / 1024), (unsigned)m->mem_kb, replaces ? "   (replaces the installed one)" : "");
    dq_text(x + 2, y + 2, line);
    dq_text(x + 2, y + 4, "It will be allowed to:");
    int row = y + 5, n = 0;
    for (size_t i = 0; i < sizeof PERMS / sizeof PERMS[0]; i++)
        if (m->perms & PERMS[i].bit) { ksnprintf(line, sizeof line, " - %s", PERMS[i].text); dq_text(x + 2, row++, line); n++; }
    if (!n) dq_text(x + 2, row++, " - nothing (it can only compute)");
    row = y + h - 4;
    ksnprintf(line, sizeof line, "from %s", url);
    dq_text(x + 2, row, line);
    dq_text(x + 2, row + 1, pinned ? "(file checked against the hash in the QR code)" : "(no hash in the QR code: trust the server)");
    dq_text(x + 2, y + h - 2, "Enter: install     Esc: cancel");
    for (;;) {
        int k = dq_getkey();
        if (k == DQK_ENTER) return true;
        if (k == DQK_ESC || k == 'q' || k == 'Q' || k == 'n' || k == 'N') return false;
    }
}

/* ---- the whole flow --------------------------------------------------------------------------- */

static void show_status(const char *a, const char *b)
{
    int w = dq_cols() * 3 / 5, h = 6, x = (dq_cols() - w) / 2, y = (dq_rows() - h) / 2;
    dq_clear();
    dq_window(x, y, w, h);
    dq_text(x + 2, y + 1, a);
    if (b) dq_text(x + 2, y + 2, b);
}

bool qrinstall_code(const char *code, char *installed_name)
{
    static char url[512], err[96];
    uint8_t want[32], got[32];
    bool pinned;
    if (installed_name) installed_name[0] = 0;
    if (!parse_code(code, url, sizeof url, want, &pinned)) {
        dq_clear();
        kprintf("install: not an app code: %s\n", code);
        dq_message("This QR code is not an app.", "(expected meos://install?url=... or an https link)");
        dq_clear();
        return false;
    }
    kprintf("install: downloading %s%s\n", url, pinned ? " (hash pinned)" : "");
    show_status("Downloading...", url);
    uint8_t *buf = kmalloc_psram(DL_MAX);
    size_t len = 0;
    if (!buf) { dq_message("Out of memory.", NULL); dq_clear(); return false; }
    if (!web_download(url, buf, DL_MAX, &len, err, sizeof err)) {
        dq_clear();
        kprintf("install: download failed: %s\n", err);
        dq_message("Download failed:", err);
        kfree(buf);
        dq_clear();
        return false;
    }
    if (pinned) {
        sha256(buf, len, got);
        if (memcmp(got, want, 32) != 0) {
            dq_clear();
            kprintf("install: hash mismatch, not installed\n");
            dq_message("The downloaded file does not match the", "hash in the QR code. Not installed.");
            kfree(buf);
            dq_clear();
            return false;
        }
    }
    wasm_manifest_t m;
    if (!wasm_read_manifest(buf, len, &m)) {
        dq_clear();
        kprintf("install: %u bytes without a valid manifest\n", (unsigned)len);
        dq_message("That is not a MeOS app", "(no valid manifest in the .wasm file).");
        kfree(buf);
        dq_clear();
        return false;
    }
    bool ok = confirm(&m, len, url, pinned, wasm_is_installed(m.name));
    bool installed = false;
    if (ok) {
        char name[16];
        int rc = wasm_install_image(buf, len, name);
        dq_clear();
        if (rc == ERR_OK) { char l[64]; ksnprintf(l, sizeof l, "Installed \"%s\". Type its name to run it.", name); dq_message(l, NULL); installed = true; if (installed_name) strcpy(installed_name, name); }
        else { char l[48]; ksnprintf(l, sizeof l, "error %d", rc); dq_message("Install failed:", l); }
    }
    kfree(buf);
    dq_clear();
    return installed;
}

bool qrinstall_run(char *installed_name)
{
    static char code[1024];
    if (installed_name) installed_name[0] = 0;
    if (!scan(code, sizeof code)) { dq_clear(); return false; }
    return qrinstall_code(code, installed_name);
}
