#include <assert.h>
#include <stdio.h>
#include <m5os/hal_mem.h>
#include <m5os/board_config.h>
uint8_t _ext_ram_bss_end;
static bool initialized = true;
static size_t physical_size = 32u * 1024u * 1024u;
static uintptr_t mapping;
static int map_error;
bool esp_psram_is_initialized(void) { return initialized; }
size_t esp_psram_get_size(void) { return physical_size; }
int esp_mmu_paddr_to_vaddr(unsigned p, int target, int type, void **out) {
    assert(!p && !target && !type); *out = (void *)mapping; return map_error;
}
static void expect(uintptr_t want_base, size_t want_size) {
    uintptr_t base; size_t size; hal_psram_region(&base, &size);
    assert(base == want_base && size == want_size);
    hal_psram_reserve_region(&base, &size);
    assert(base == (want_size ? want_base + want_size : 0));
    assert(size == (want_size ? M5OS_PSRAM_RESERVE : 0));
}
int main(void) {
    uintptr_t end = ((uintptr_t)&_ext_ram_bss_end + 63u) & ~(uintptr_t)63u;
    mapping = end - 4096;
    size_t usable = physical_size - M5OS_PSRAM_RESERVE;
#if CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY
    expect(end, usable - 4096);
    mapping = end - usable; expect(0, 0); /* BSS consumes the complete MeOS slice. */
    mapping = end + 64; expect(mapping, usable); /* BSS below mapping. */
#else
    expect(mapping, usable);
#endif
    initialized = false; expect(0, 0); initialized = true;
    map_error = 1; expect(0, 0); map_error = 0;
    mapping = 0; expect(0, 0); mapping = end;
    physical_size = M5OS_PSRAM_RESERVE; expect(0, 0);
    physical_size = M5OS_PSRAM_RESERVE - 1; expect(0, 0);
    puts("PSRAM: BSS boundaries, reserved tail, mapping failures and small RAM PASS");
}
