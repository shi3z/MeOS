#pragma once
/*
 * The MeOS WebAssembly ABI, version 1: the functions a .wasm app can import from the host module "meos".
 * Freestanding C for clang --target=wasm32 -nostdlib (tools/wasmbuild.sh).  Every function checks the permission
 * the app declared in its manifest and returns a negative ERR_* code (-16 = no permission) instead of trapping.
 *
 * The manifest is a text file next to the source (NAME.manifest: name=, perms=, mem=, version=; see m5os/wasm.h);
 * tools/wasmbuild.sh adds it to the .wasm as the custom section "meos.manifest".
 *
 *   permission  letters in `perms=`:  C console   D display   I input   r read files   w write files   s spawn   m messages
 */
#define MEOS_IMPORT(n) __attribute__((import_module("meos"), import_name(n)))

/* C */ MEOS_IMPORT("console_write") int meos_console_write(const char *utf8, int len);
/* C */ MEOS_IMPORT("console_read")  int meos_console_read(void);              /* a byte, or -1 if none is waiting */
        MEOS_IMPORT("sleep_ms")      void meos_sleep_ms(unsigned ms);
        MEOS_IMPORT("uptime_ms")     unsigned meos_uptime_ms(void);

/* r/w */ MEOS_IMPORT("file_open")   int meos_file_open(const char *path, int len, unsigned flags);   /* flags: 1 read 2 write 4 create 8 truncate 16 append */
          MEOS_IMPORT("file_read")   int meos_file_read(int h, void *buf, int len);
          MEOS_IMPORT("file_write")  int meos_file_write(int h, const void *buf, int len);
          MEOS_IMPORT("file_close")  int meos_file_close(int h);

/* D */ MEOS_IMPORT("display_open")  int meos_display_open(void);              /* take the screen over (cleared); the console is hidden meanwhile */
/* D */ MEOS_IMPORT("display_close") int meos_display_close(void);             /* give it back (done automatically when the app ends) */
/* D */ MEOS_IMPORT("display_present") int meos_display_present(void);          /* double buffering: drawing is off-screen until this (also done by sleep_ms and console_read) */
/* D */ MEOS_IMPORT("display_line")  int meos_display_line(int x0, int y0, int x1, int y1, unsigned rgb565);
/* D */ MEOS_IMPORT("display_tri")   int meos_display_tri(int x0, int y0, int x1, int y1, int x2, int y2, unsigned rgb565);   /* filled */
/* D */ MEOS_IMPORT("display_blit")  int meos_display_blit(int x, int y, int w, int h, const unsigned short *rgb565, int scale);   /* w,h <= 256, 2-byte aligned */
/* D */ MEOS_IMPORT("display_size")  int meos_display_size(void);              /* width | height << 16 */
/* D */ MEOS_IMPORT("display_fill")  int meos_display_fill(int x, int y, int w, int h, unsigned rgb565);
/* D */ MEOS_IMPORT("display_text")  int meos_display_text(int x, int y, const char *utf8, int len, unsigned fg, unsigned bg, int scale);

static inline int meos_strlen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static inline void meos_puts(const char *s) { meos_console_write(s, meos_strlen(s)); }

#define MEOS_RGB(r, g, b) ((unsigned)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) & 0xF8) >> 3))

/* Keys from the console: a character, or one of these for the arrow keys; -1 when nothing is waiting. */
enum { MEOS_KEY_NONE = -1, MEOS_KEY_UP = 0x100, MEOS_KEY_DOWN, MEOS_KEY_RIGHT, MEOS_KEY_LEFT };
static inline int meos_getkey(void)
{
    int c = meos_console_read();
    if (c != 0x1B) return c;
    int c2 = meos_console_read(), c3 = meos_console_read();
    if (c2 == '[' || c2 == 'O') {
        if (c3 == 'A') return MEOS_KEY_UP;
        if (c3 == 'B') return MEOS_KEY_DOWN;
        if (c3 == 'C') return MEOS_KEY_RIGHT;
        if (c3 == 'D') return MEOS_KEY_LEFT;
    }
    return 0x1B;
}

/* A tiny random generator (no libc here). */
static unsigned meos_seed_ = 12345u;
static inline unsigned meos_rand(void) { meos_seed_ = meos_seed_ * 1664525u + 1013904223u; return meos_seed_ >> 8; }
