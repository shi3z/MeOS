#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../drivers/lcd/bizud.c"

static uint8_t image[32 + sizeof(biz_glyph) + 4 + sizeof(biz_glyph) + 16];
static size_t position;
static unsigned opens, allocations, frees;
static bool fail_scratch;

static void put32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static void make_image(void)
{
    memcpy(image, "M5BIZ24\0", 8);
    put32(image + 8, 1);
    put32(image + 12, 1);
    put32(image + 16, sizeof(biz_glyph));
    put32(image + 20, 4 + sizeof(biz_glyph) + 16);
    biz_glyph ordinary = {.cp = 'A', .offset = 0, .length = 1,
                          .x = 0, .y = -1, .w = 1, .h = 1, .advance = 7};
    biz_glyph vertical = {.cp = 'A', .offset = 1, .length = 1,
                          .x = 0, .y = -1, .w = 1, .h = 1, .advance = 9};
    memcpy(image + 32, &ordinary, sizeof ordinary);
    uint8_t *data = image + 32 + sizeof ordinary;
    data[0] = 0xf0; data[1] = 0xe0;
    memcpy(data + 4, &vertical, sizeof vertical);
    memcpy(data + 4 + sizeof vertical, "M5VRT2\0\0", 8);
    put32(data + 4 + sizeof vertical + 8, 1);
    put32(data + 4 + sizeof vertical + 12, 4);
    put32(image + 24, crc32(image + 32, sizeof ordinary));
    put32(image + 28, crc32(data, 4 + sizeof vertical + 16));
}

void *kmalloc_psram(size_t size)
{
    if (fail_scratch && size == BIZUD_SCRATCH_BYTES) return NULL;
    void *p = malloc(size);
    if (p) allocations++;
    return p;
}

void kfree(void *p)
{
    if (p) {
        frees++;
        free(p);
    }
}

int vfs_open(const char *path, uint32_t flags)
{
    assert(!strcmp(path, BIZUD_PATH));
    assert(flags == VFS_O_READ);
    position = 0;
    opens++;
    return 3;
}

int vfs_read(int fd, void *out, size_t size)
{
    assert(fd == 3);
    if (position + size > sizeof image) return -1;
    memcpy(out, image + position, size);
    position += size;
    return (int)size;
}

int vfs_close(int fd) { assert(fd == 3); return 0; }

int vfs_stat(const char *path, vfs_info_t *info)
{
    assert(!strcmp(path, BIZUD_PATH));
    memset(info, 0, sizeof *info);
    info->type = FS_TYPE_FILE;
    info->size = sizeof image;
    return 0;
}

uint64_t hal_time_now_us(void)
{
    static uint64_t now;
    now += 1000;
    return now;
}

void kprintf(const char *format, ...)
{
    (void)format;
}

void display_draw_rect(int x, int y, int w, int h, uint16_t color)
{
    (void)x; (void)y; (void)w; (void)h; (void)color;
}

void display_draw_bitmap(int x, int y, int w, int h, const uint16_t *pixels)
{
    (void)x; (void)y; (void)w; (void)h; (void)pixels;
}

int main(void)
{
    make_image();
    const size_t expected = 2 * sizeof(biz_glyph) + 4 + 16 + BIZUD_SCRATCH_BYTES;

    assert(bizud_begin());
    assert(opens == 1 && allocations == 2 && frees == 0);
    assert(bizud_cache_users() == 1 && bizud_cache_bytes() == expected);
    assert(bizud_width('A') == 7);
    assert(bizud_vertical_advance('A') == 9);
    assert(bizud_vertical_advance('B') == 24);

    assert(bizud_begin());
    assert(bizud_cache_users() == 2 && opens == 1 && allocations == 2);
    bizud_end();
    assert(bizud_trim() == 0 && bizud_cache_users() == 1);
    bizud_end();
    assert(bizud_cache_users() == 0 && bizud_cache_bytes() == expected && frees == 0);

    assert(bizud_begin());
    assert(opens == 1 && allocations == 2);
    bizud_end();
    assert(bizud_trim() == expected);
    assert(!bizud_cache_bytes() && frees == 2);

    fail_scratch = true;
    assert(!bizud_begin());
    assert(opens == 2 && allocations == 3 && frees == 3);
    assert(!bizud_cache_bytes() && !bizud_cache_users());
    fail_scratch = false;

    assert(bizud_begin());
    assert(opens == 3 && allocations == 5);
    bizud_end();
    assert(bizud_trim() == expected && frees == 5);
    puts("BIZ UD cache: leases, warm reuse, trim and allocation failure PASS");
    return 0;
}
