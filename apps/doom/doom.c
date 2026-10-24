#include <m5os/board_config.h>
#include <m5os/doom.h>
#if M5OS_DOOM
#include <m5os/app.h>
#include <m5os/arch.h>
#include <m5os/display.h>
#include <m5os/hal_time.h>
#include <m5os/kmalloc.h>
#include <m5os/kprintf.h>
#include <m5os/vfs.h>
#include <m5os/ime.h>
#include <m5os/errno.h>
#include "../../drivers/lcd/fbcon.h"
#include "../../drivers/tab5kbd/tab5touch.h"
#include "doomgeneric.h"
#include "doomkeys.h"
#include "i_video.h"
#include "d_loop.h"
#include "runtime.h"
#include "layout.h"
#include <string.h>
#include <setjmp.h>
#ifdef DOOM_EMBED_RAW_SIZE
#include "miniz.h"
extern const uint8_t doom_wad_z_start[] asm("_binary_doom_wad_z_start");
extern const uint8_t doom_wad_z_end[] asm("_binary_doom_wad_z_end");
#endif

uint32_t doom_owner;
uint8_t *doom_iwad;
size_t doom_iwad_size;
char doom_iwad_path[96];
static char s_requested[96];
static bool s_pending, s_screen, s_silent;
static unsigned s_demo_seconds;
static uint32_t s_started, s_frames;
static uint8_t *s_hud_cache;
static uint16_t s_hud_palette[256];
static bool s_hud_valid;
static uint64_t s_rgb_us, s_blit_us, s_input_us, s_wait_us, s_profile_at, s_copy_us, s_flush_us;
static uint32_t s_profile_frame, s_unique_frames;
static int s_previous_gametic;
void doom_profile_input(uint32_t us) { s_input_us+=us; }
static jmp_buf s_exit;
extern uint8_t _doom_bss_start[], _doom_bss_end[], _doom_common_start[], _doom_common_end[];
extern uint8_t _doom_data_start[], _doom_data_end[];
static uint8_t *s_initial_data; /* kernel-owned snapshot persists across game instances */

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static bool valid_wad(void)
{
    if (!doom_iwad || doom_iwad_size < 12 || memcmp(doom_iwad, "IWAD", 4)) return false;
    uint32_t count = le32(doom_iwad + 4), off = le32(doom_iwad + 8);
    if (!count || count > 32768 || off > doom_iwad_size || count > (doom_iwad_size - off) / 16) return false;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t start = le32(doom_iwad + off + i * 16), size = le32(doom_iwad + off + i * 16 + 4);
        if (start > doom_iwad_size || size > doom_iwad_size - start) return false;
    }
    return true;
}
static bool load_wad(void)
{
    if (!s_requested[0]) {
#ifdef DOOM_EMBED_RAW_SIZE
        doom_iwad_size = DOOM_EMBED_RAW_SIZE;
        doom_iwad = kmalloc_psram_owned(doom_iwad_size, doom_owner);
        if (!doom_iwad) return false;
        size_t got = tinfl_decompress_mem_to_mem(doom_iwad, doom_iwad_size, doom_wad_z_start,
                          (size_t)(doom_wad_z_end - doom_wad_z_start), TINFL_FLAG_PARSE_ZLIB_HEADER);
        if (got != doom_iwad_size) return false;
        strcpy(doom_iwad_path, "/doom1.wad");
        return valid_wad();
#else
        strcpy(s_requested, "/sd/DOOM1.WAD");
#endif
    }
    vfs_info_t info;
    if (vfs_stat(s_requested, &info) != ERR_OK || info.type != FS_TYPE_FILE || info.size < 12 || info.size > 12u * 1024 * 1024) return false;
    int fd = vfs_open(s_requested, VFS_O_READ); if (fd < 0) return false;
    doom_iwad_size = info.size;
    doom_iwad = kmalloc_psram_owned(doom_iwad_size, doom_owner);
    if (!doom_iwad) { vfs_close(fd); return false; }
    size_t pos = 0;
    while (pos < doom_iwad_size) {
        size_t n = doom_iwad_size - pos; if (n > 4096) n = 4096;
        int got = vfs_read(fd, doom_iwad + pos, n);
        if (got <= 0) break;
        pos += got; task_yield();
    }
    vfs_close(fd);
    strcpy(doom_iwad_path, s_requested);
    return pos == doom_iwad_size && valid_wad();
}
static void reset_engine(void)
{
    memset(_doom_bss_start, 0, (size_t)(_doom_bss_end - _doom_bss_start));
    memset(_doom_common_start, 0, (size_t)(_doom_common_end - _doom_common_start));
    memcpy(_doom_data_start, s_initial_data, (size_t)(_doom_data_end - _doom_data_start));
}
void doom_exit(int code) { longjmp(s_exit, code ? 2 : 1); }

void doom_main(void *arg)
{
    (void)arg;
    if (!display_is_ready()) { s_pending=false; kputs("doom: display is not initialized; restart the board before playing\n"); return; }
    uint32_t owner = task_current()->app->app_id;
    uint32_t irq = arch_irq_disable();
    if (doom_owner || fbcon_is_paused()) { s_pending = false; arch_irq_restore(irq); kputs("doom: display/game busy\n"); return; }
    doom_owner = owner; s_pending = false;
    arch_irq_restore(irq);
    if (!display_try_claim(&doom_owner)) { kputs("doom: display is already in use\n"); return; }
    size_t data_size = (size_t)(_doom_data_end - _doom_data_start);
    if (!data_size || data_size > 256u * 1024u || (uintptr_t)_doom_bss_end <= (uintptr_t)_doom_bss_start) {
        kputs("doom: invalid engine linker state\n"); return;
    }
    if (!s_initial_data) {
        s_initial_data = kmalloc_psram(data_size);
        if (!s_initial_data) { kputs("doom: no PSRAM for engine snapshot\n"); return; }
        memcpy(s_initial_data, _doom_data_start, data_size);
    }
    reset_engine();
    if (!load_wad()) { kputs("doom: IWAD unavailable/invalid or insufficient PSRAM; use doom /sd/DOOM1.WAD\n"); return; }
    s_hud_cache=kmalloc_psram_owned(320u*32u,owner); s_hud_valid=false;
    s_started = (uint32_t)(hal_time_now_us() / 1000); s_frames = 0;
    doom_input_reset(); doom_sound_reset();
    kprintf("doom: native 320x200, IWAD %u bytes; Q/Ctrl-C or touch QUIT exits\n", (unsigned)doom_iwad_size);
    kconsole_set_focus(task_current()->id);
    ime_bypass(true); fbcon_pause(true); s_screen = true;
    task_sleep(60);
    int result = setjmp(s_exit);
    if (result == 0) {
        char *args[] = { "doom", "-iwad", doom_iwad_path, "-nomouse", "-warp", "1", "1", NULL, NULL };
        if (s_silent) args[7] = "-nosound";
        doomgeneric_Create(s_silent ? 8 : 7, args);
        for (;;) { doomgeneric_Tick(); task_yield(); }
    }
    kprintf("doom: %s after %u frames\n", result == 1 ? "stopped" : "engine error", s_frames);
}
void doom_app_cleanup(uint32_t owner)
{
    if (!doom_owner || owner != doom_owner) return;
    doom_sound_shutdown();
    if (s_screen) {
        display_set_target(NULL); ime_bypass(false); kconsole_set_focus(0); fbcon_pause(false);
    }
    display_release_claim(&doom_owner);
    s_screen = false; s_hud_cache=NULL; s_hud_valid=false; doom_iwad = NULL; doom_iwad_size = 0;
    s_requested[0] = 0; s_demo_seconds = 0; s_silent = false;
    if (s_initial_data) reset_engine();
    doom_owner = 0;
}
int doom_command(int argc, char **argv)
{
    if (doom_owner || s_pending) { kputs("doom: already running\n"); return 1; }
    s_requested[0] = 0; s_demo_seconds = 0; s_silent = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--silent")) s_silent = true;
        else if (!strcmp(argv[i], "--demo") && i + 1 < argc) {
            unsigned sec = 0;
            for (const char *p = argv[++i]; *p; p++) { if (*p < '0' || *p > '9' || sec > 60) goto usage; sec = sec * 10 + *p - '0'; }
            if (!sec || sec > 600) goto usage;
            s_demo_seconds = sec;
        } else if (!s_requested[0] && argv[i][0] == '/' && strlen(argv[i]) < sizeof s_requested) strcpy(s_requested, argv[i]);
        else goto usage;
    }
    uint32_t id;
    s_pending = true;
    int rc = app_start("doom", &id);
    if (rc != ERR_OK) { s_pending = false; kprintf("doom: cannot start (%d)\n", rc); return 1; }
    kprintf("doom: app %u started\n", id); return 0;
usage:
    kputs("usage: doom [/sd/DOOM1.WAD] [--demo 1..600 seconds] [--silent]\n"); return 1;
}

void DG_Init(void)
{
    display_set_target(NULL); display_clear(COLOR_BLACK);
    display_fill_rect(0,0,160,720,RGB565(16,22,30));
    display_fill_rect(1120,0,160,720,RGB565(16,22,30));
    for (unsigned i = 0; i < DOOM_BUTTON_COUNT; i++) {
        const doom_button_t *b=&doom_buttons[i];
        uint16_t bg=!b->key ? RGB565(100,35,35) : b->key==KEY_FIRE ? RGB565(125,60,25) : RGB565(40,55,75);
        int scale=b->key==KEY_FIRE || strlen(b->label)==1 ? 3 : 2;
        display_fill_rect(b->x,b->y,b->w,b->h,bg);
        display_draw_text(b->x+(b->w-(int)strlen(b->label)*8*scale)/2,
                          b->y+(b->h-8*scale)/2,b->label,COLOR_WHITE,bg,scale);
    }
}
uint32_t DG_GetTicksMs(void)
{
    uint32_t now = (uint32_t)(hal_time_now_us() / 1000u) - s_started + 1;
    if (s_demo_seconds && now >= s_demo_seconds * 1000u) doom_exit(0);
    return now;
}
void DG_SleepMs(uint32_t ms)
{
    uint64_t start=hal_time_now_us(); task_sleep(ms ? ms : 1);
    s_wait_us+=hal_time_now_us()-start;
}
void DG_SetWindowTitle(const char *title) { kprintf("doom: %s\n", title); }
void DG_DrawFrame(void)
{
    uint64_t start=hal_time_now_us();
    /* The HUD is often identical even while moving through the world. Preserve
     * its existing pixels only when both indices and palette match exactly. */
    bool hud_changed=!s_hud_cache || !s_hud_valid ||
        memcmp(s_hud_palette,doom_video_palette,sizeof s_hud_palette) ||
        memcmp(s_hud_cache,I_VideoBuffer+320*168,320*32);
    if(hud_changed && s_hud_cache) {
        memcpy(s_hud_cache,I_VideoBuffer+320*168,320*32);
        memcpy(s_hud_palette,doom_video_palette,sizeof s_hud_palette); s_hud_valid=true;
    }
    uint64_t converted=hal_time_now_us();
    display_draw_indexed_bitmap_prefix(DOOM_VIEW_X, DOOM_VIEW_Y, 320, 200,
        I_VideoBuffer, doom_video_palette, DOOM_VIEW_W, DOOM_VIEW_H,hud_changed?720:605);
    uint64_t drawn=hal_time_now_us();
    uint32_t copy,flush; display_last_present_times(&copy,&flush); s_copy_us+=copy; s_flush_us+=flush;
    s_rgb_us+=converted-start; s_blit_us+=drawn-converted;
    s_frames++;
    if(gametic!=s_previous_gametic) { s_unique_frames++; s_previous_gametic=gametic; }
    if (s_frames == 1 || !(s_frames % 100)) kprintf("doom: frame %u, elapsed %u ms\n", s_frames, DG_GetTicksMs());
    if (s_frames==1 || s_frames-s_profile_frame>=100) {
        if (s_frames>1) {
            unsigned n=s_frames-s_profile_frame;
            unsigned fps10=(unsigned)((uint64_t)n*10000000/(drawn-s_profile_at));
            kprintf("doom profile: %u.%u fps; frame %u us, prep %u, blit %u, touch %u, wait %u us/frame\n",
                fps10/10,fps10%10,(unsigned)((drawn-s_profile_at)/n),
                (unsigned)(s_rgb_us/n),(unsigned)(s_blit_us/n),(unsigned)(s_input_us/n),(unsigned)(s_wait_us/n));
        }
        if(s_frames>1) kprintf("doom present: copy %u us, flush %u us/frame\n",(unsigned)(s_copy_us/(s_frames-s_profile_frame)),(unsigned)(s_flush_us/(s_frames-s_profile_frame)));
        if(s_frames>1) kprintf("doom content: %u.%u distinct game frames/s\n",(unsigned)((uint64_t)s_unique_frames*10000000/(drawn-s_profile_at))/10,(unsigned)((uint64_t)s_unique_frames*10000000/(drawn-s_profile_at))%10);
        s_unique_frames=0;
        s_copy_us=s_flush_us=0;
        s_profile_at=drawn; s_profile_frame=s_frames;
        s_rgb_us=s_blit_us=s_input_us=s_wait_us=0;
    }
    if (s_demo_seconds && DG_GetTicksMs() >= s_demo_seconds * 1000u) doom_exit(0);
}
#else
void doom_app_cleanup(uint32_t app_id) { (void)app_id; }
void doom_main(void *arg) { (void)arg; }
int doom_command(int argc, char **argv) { (void)argc; (void)argv; return -4; }
#endif
