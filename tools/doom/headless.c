/* Run the unmodified engine with MeOS's private libc adapter, without hardware or an OS shim. */
#define DOOM_COMPAT_DECLARATIONS_ONLY
#include "../../components/doomengine/compat.h"
#include "doomgeneric.h"
#include "i_video.h"
extern uint16_t doom_video_palette[256];
#include <assert.h>
#include <setjmp.h>
#include <m5os/types.h>

uint32_t doom_owner = 1;
uint8_t *doom_iwad;
size_t doom_iwad_size;
char doom_iwad_path[96] = "/doom1.wad";
static jmp_buf done;
static unsigned ticks, frames;
static const char *output;
typedef struct mem { struct mem *next; } mem_t;
static mem_t *memories;
void *kmalloc_psram_owned(size_t n, uint32_t owner)
{
    assert(owner == 1); mem_t *p = malloc(sizeof *p + n); assert(p); p->next = memories; memories = p; return p + 1;
}
void kfree(void *ptr)
{
    mem_t *p = (mem_t *)ptr - 1;
    for (mem_t **it = &memories; *it; it = &(*it)->next) if (*it == p) { *it = p->next; free(p); return; }
    assert(!"unknown allocation");
}
void kputs(const char *s) { fputs(s, stdout); }
void kprintf(const char *s, ...) { (void)s; }
void doom_exit(int code) { longjmp(done, code ? 2 : 1); }
void DG_Init(void) { }
void DG_SetWindowTitle(const char *t) { (void)t; }
uint32_t DG_GetTicksMs(void) { return ++ticks; }
void DG_SleepMs(uint32_t ms) { ticks += ms; }
int DG_GetKey(int *down, unsigned char *key) { (void)down; (void)key; return 0; }
void DG_DrawFrame(void)
{
    if (++frames < 120) return;
    FILE *f = fopen(output, "wb"); assert(f); fprintf(f, "P6\n320 200\n255\n");
    unsigned changed = 0;
    for (unsigned i = 0; i < 320u * 200u; i++) {
        uint16_t c = doom_video_palette[I_VideoBuffer[i]];
        uint8_t rgb[3] = { (c >> 8)&0xf8, (c >> 3)&0xfc, (c << 3)&0xf8 };
        assert(fwrite(rgb, 1, 3, f) == 3);
        if (i && c != doom_video_palette[I_VideoBuffer[i - 1]]) changed++;
    }
    fclose(f); assert(changed > 1000); doom_exit(0);
}
int main(int argc, char **argv)
{
    assert(argc == 3); output = argv[2];
    FILE *f = fopen(argv[1], "rb"); assert(f); fseek(f, 0, SEEK_END); doom_iwad_size = ftell(f); rewind(f);
    doom_iwad = malloc(doom_iwad_size); assert(doom_iwad && fread(doom_iwad, 1, doom_iwad_size, f) == doom_iwad_size); fclose(f);
    char *args[] = { "doom", "-iwad", doom_iwad_path, "-nosound", "-nomouse", "-warp", "1", "1", NULL };
    int status = setjmp(done);
    if (!status) { doomgeneric_Create(8, args); for (;;) doomgeneric_Tick(); }
    assert(status == 1 && frames == 120);
    while (memories) { mem_t *next = memories->next; free(memories); memories = next; }
    free(doom_iwad);
    puts("Doom engine headless: 120 textured frames rendered PASS");
}
