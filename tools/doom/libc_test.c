#define DOOM_COMPAT_DECLARATIONS_ONLY
#include "../../components/doomengine/compat.h"
#include <assert.h>
#include <setjmp.h>
#include <m5os/types.h>

uint32_t doom_owner = 7;
uint8_t *doom_iwad;
size_t doom_iwad_size;
char doom_iwad_path[96] = "/doom1.wad";
static jmp_buf failure;
static int allocations;
void *kmalloc_psram_owned(size_t n, uint32_t owner) { assert(owner == 7); allocations++; return malloc(n); }
void kfree(void *p) { allocations--; free(p); }
void kputs(const char *s) { (void)s; }
void kprintf(const char *s, ...) { (void)s; }
void doom_exit(int c) { longjmp(failure, c ? c : 1); }
int main(void)
{
    void *p = doom_calloc(8, 8); assert(p);
    for (int i = 0; i < 64; i++) assert(((uint8_t *)p)[i] == 0);
    memset(p, 0x6a, 64); p = doom_realloc(p, 128);
    for (int i = 0; i < 64; i++) assert(((uint8_t *)p)[i] == 0x6a);
    doom_free(p); char *s = doom_strdup("hello"); assert(!strcmp(s, "hello")); doom_free(s);
    uint8_t wad[] = "IWADpayload"; doom_iwad = wad; doom_iwad_size = sizeof wad - 1;
    assert(!doom_fopen("/secret", "rb") && !doom_fopen("/doom1.wad", "wb"));
    FILE *f = doom_fopen("/doom1.wad", "rb"); assert(f);
    char buf[128]; assert(doom_fread(buf, 1, 4, f) == 4 && !memcmp(buf, "IWAD", 4));
    assert(doom_fseek(f, -3, SEEK_END) == 0 && doom_ftell(f) == 8);
    assert(doom_fread(buf, 1, 12, f) == 3 && !memcmp(buf, "oad", 3) && doom_feof(f));
    assert(doom_fwrite(buf, 1, 1, f) == 0 && doom_ferror(f));
    assert(doom_fseek(f, -100, SEEK_SET) == -1 && doom_fseek(f, 1, SEEK_END) == -1);
    assert(doom_fread(buf, 2, SIZE_MAX, f) == 0);
    assert(doom_fclose(f) == 0);
    assert(doom_snprintf(buf, sizeof buf, "%s %.3s %i %08x %zu", "a", "hello", -42, 12u, (size_t)9) == (int)strlen("a hel -42 0000000c 9"));
    assert(!strcmp(buf, "a hel -42 0000000c 9"));
    assert(doom_snprintf(buf, sizeof buf, "STCFN%.3d", 33) == 8 && !strcmp(buf, "STCFN033"));
    assert(doom_snprintf(buf, 4, "abcdef") == 6 && !strcmp(buf, "abc"));
    int value = 0; assert(doom_sscanf(" 0x1a", " 0x%x", &value) == 1 && value == 26);
    assert(doom_sscanf("-42", "%d", &value) == 1 && value == -42);
    assert(doom_sscanf("oops", "%d", &value) == 0);
    if (!setjmp(failure)) { doom_calloc(SIZE_MAX, 2); assert(!"overflow accepted"); }
    assert(!allocations);
    puts("Doom private libc: memory, read-only IWAD, bounds and formatting tests PASS");
}
