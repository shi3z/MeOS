/* Host-side test of the whole input method: feeds key presses, checks the text that comes out and shows the overlay.
 *   tools/ime/host_test.sh [path to an SKK dictionary blob made by tools/ime_mkdict.py] */
#include <m5os/ime.h>
#include <m5os/kprintf.h>
#include <m5os/task.h>
#include <m5os/display.h>
#include <m5os/kmalloc.h>
#include "../../drivers/lcd/fbcon.h"
#include "../../drivers/ime/ime_dict.h"
#include "../../drivers/ime/ime_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* ---- stubs for the OS ---- */
uint32_t arch_irq_disable(void) { return 0; }
void arch_irq_restore(uint32_t state) { (void)state; }
static char s_out[4096];
static size_t s_outn;
size_t kconsole_inject_n(const char *s, size_t n) { for (size_t i = 0; i < n; i++) s_out[s_outn++] = s[i]; s_out[s_outn] = 0; return n; }
void task_sleep(uint32_t ms) { (void)ms; }
void *kmalloc_psram(size_t n) { return malloc(n); }
int ksnprintf(char *b, size_t cap, const char *fmt, ...) { va_list a; va_start(a, fmt); int r = vsnprintf(b, cap, fmt, a); va_end(a); return r; }
void kprintf(const char *fmt, ...) { va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a); }
void ime_dict_load_async(void) {}
void display_draw_codepoints(int x, int y, const uint16_t *c, int n, uint16_t fg, uint16_t bg, int s) { (void)x; (void)y; (void)c; (void)n; (void)fg; (void)bg; (void)s; }
void display_draw_bitmap(int x, int y, int w, int h, const uint16_t *p) { (void)x; (void)y; (void)w; (void)h; (void)p; }
void display_read_bitmap(int x, int y, int w, int h, uint16_t *p) { (void)x; (void)y; (void)w; (void)h; (void)p; }
bool fbcon_is_paused(void) { return false; }
void fbcon_overlay_colors(int s, uint16_t *fg, uint16_t *bg) { (void)s; *fg = *bg = 0; }
void fbcon_get_geometry(int *r, int *c, int *cr, int *cc) { *r = 45; *c = 80; *cr = 3; *cc = 10; }

static char s_view[2048];
void fbcon_overlay_begin(void) { s_view[0] = 0; }
void fbcon_overlay_text(int anchor, int row, int col, const uint16_t *cps, int n, int style)
{
    char u[256];
    ime_cps_to_u8(cps, n, u, sizeof u);
    char line[300];
    snprintf(line, sizeof line, "  [%c r%d c%d s%d] %s\n", anchor == FBCON_ANCHOR_CURSOR ? 'C' : 'S', row, col, style, u);
    strcat(s_view, line);
}
void fbcon_overlay_end(void) {}

extern const uint8_t ime_mini_dict[];
extern const uint32_t ime_mini_dict_size;

static int fails;
static bool strict = true;          /* exact expectations only hold with the built-in dictionary */
static void keys(const char *seq)             /* characters, or "<name>" for special keys */
{
    for (const char *p = seq; *p; p++) {
        char k[8] = {0};
        if (*p == '<') {
            const char *e = strchr(p, '>');
            char name[16] = {0};
            memcpy(name, p + 1, (size_t)(e - p - 1));
            p = e;
            if (!strcmp(name, "toggle")) strcpy(k, IME_TOGGLE_KEY);
            else if (!strcmp(name, "enter")) strcpy(k, "\r");
            else if (!strcmp(name, "bs")) strcpy(k, "\x7f");
            else if (!strcmp(name, "esc")) strcpy(k, "\x1b");
            else if (!strcmp(name, "left")) strcpy(k, "\x1b[D");
            else if (!strcmp(name, "right")) strcpy(k, "\x1b[C");
            else if (!strcmp(name, "up")) strcpy(k, "\x1b[A");
            else if (!strcmp(name, "down")) strcpy(k, "\x1b[B");
            else if (!strcmp(name, "sleft")) strcpy(k, "\x1b[1;2D");
            else if (!strcmp(name, "sright")) strcpy(k, "\x1b[1;2C");
            else if (!strcmp(name, "f6")) strcpy(k, "\x1b[17~");
            else if (!strcmp(name, "f7")) strcpy(k, "\x1b[18~");
            else if (!strcmp(name, "f10")) strcpy(k, "\x1b[21~");
            else { printf("unknown key %s\n", name); exit(2); }
        } else {
            k[0] = *p;
        }
        ime_feed(k);
    }
}

static void expect(const char *name, const char *seq, const char *want, bool show)
{
    s_outn = 0; s_out[0] = 0;
    keys(seq);
    if (show) printf("%s: out=\"%s\" view:\n%s", name, s_out, s_view);
    if (strict && strcmp(s_out, want)) { printf("FAIL %s: got \"%s\" want \"%s\"\n", name, s_out, want); fails++; }
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        strict = false;
        FILE *f = fopen(argv[1], "rb");
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        uint8_t *b = malloc((size_t)sz);
        if (fread(b, 1, (size_t)sz, f) != (size_t)sz) return 1;
        ime_dict_add(b, (uint32_t)sz);
    }
    ime_init();
    expect("off passes keys", "abc", "abc", false);
    expect("shift-arrow normalised", "<sleft>", "\x1b[D", false);
    expect("toggle on, kana", "<toggle>nihongo<enter>", "にほんご", false);
    expect("kata mode", "<f7>konpyu-ta-<enter>", "コンピューター", false);
    expect("back to hira", "<f6>aiueo<enter>", "あいうえお", false);
    expect("space idle", " ", " ", false);
    expect("conv sentence", "watashihagakuseidesu <enter>", "私は学生です", false);
    expect("window", "kanji  ", "", true);      /* list open: two Space presses */
    keys("<esc><esc>");
    expect("punct", "arigatou,<enter>", "ありがとう、", false);
    expect("bs pending", "kaz<bs><bs>a<enter>", "あ", false);
    expect("esc cancels", "kanji<esc>", "", false);
    expect("candidates", "kanji <down><enter>", "感じ", true);
    expect("literal capital", "Hll<enter>", "Hll", false);
    expect("resize", "watashihagakuseidesu <sleft><enter>", "私は学生です", false);
    expect("move focus", "watashihagakuseidesu <right> <enter>", "私はがくせいです", false);
    expect("toggle commits", "ni<toggle>", "に", false);
    expect("off again", "x", "x", false);
    expect("f10", "<toggle>abc<f10><enter><toggle>", "abc", false);
    ime_set_app_view(true);
    expect("app commit bypasses byte stream", "<toggle>nihongo<enter>", "", false);
    char commit[IME_COMMIT_BYTES];
    size_t committed=ime_take_commit(commit,sizeof commit);
    const char *want_commit="にほんご";
    if(strict&&(committed!=strlen(want_commit)||memcmp(commit,want_commit,committed))) {
        printf("FAIL app commit: got %zu bytes\n",committed);fails++;
    }
    if(ime_take_commit(commit,sizeof commit)!=0) {printf("FAIL app commit delivered twice\n");fails++;}
    expect("small destination leaves commit pending", "ai<enter>", "", false);
    if(ime_take_commit(commit,1)!=0) {printf("FAIL short app commit buffer accepted\n");fails++;}
    committed=ime_take_commit(commit,sizeof commit);
    if(strict&&(committed!=strlen("あい")||memcmp(commit,"あい",committed))) {
        printf("FAIL pending app commit: got %zu bytes\n",committed);fails++;
    }
    ime_set_app_view(false);
    expect("app view disabled", "<toggle>x", "x", false);
    printf(fails ? "ime: %d FAIL\n" : "ime: all pass\n", fails);
    return fails != 0;
}
