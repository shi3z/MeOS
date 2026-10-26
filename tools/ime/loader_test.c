#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <m5os/errno.h>
#include <m5os/fs.h>
#include <m5os/task.h>
#include <m5os/vfs.h>
#include "../../drivers/ime/ime_dict.h"

void ime_dict_load_async(void);

static unsigned char data[6u * 1024u * 1024u];
static size_t length, position, bytes_since_nap, reads, naps;
static int stat_rc, open_rc, close_rc, closes, frees;
static fs_type_t type;
static bool no_memory;
static int read_fault;
static unsigned char *allocation;
static void (*entry)(void *);
static char receipt[2048];
static task_t loader = {.id = 42};

static int candidates(void)
{
    char out[64];
    int n = ime_dict_lookup("test", out, sizeof out);
    if (n) assert(n == 1 && !strcmp(out, "candidate"));
    return n;
}

int vfs_stat(const char *path, vfs_info_t *info)
{
    assert(!strcmp(path, "/assets/ime-dict.bin"));
    *info = (vfs_info_t){.type = type, .size = (uint32_t)length};
    return stat_rc;
}
int vfs_open(const char *path, uint32_t flags)
{
    assert(!strcmp(path, "/assets/ime-dict.bin") && flags == VFS_O_READ);
    return open_rc < 0 ? open_rc : 3;
}
int vfs_read(int fd, void *buf, size_t n)
{
    assert(fd == 3 && n && n <= 8192 && n <= length - position);
    assert(!candidates());
    reads++;
    if (read_fault && position >= 8192) {
        if (read_fault == 1) return ERR_IO;
        if (read_fault == 2) return 0;
        if (read_fault == 3) return (int)n - 1;
        return (int)n + 1;
    }
    memcpy(buf, data + position, n);
    position += n;
    bytes_since_nap += n;
    assert(bytes_since_nap <= 64u * 1024u);
    return (int)n;
}
int vfs_close(int fd)
{
    assert(fd == 3 && !candidates());
    closes++;
    return close_rc;
}
void *kmalloc_psram(size_t n)
{
    assert(n == length && !allocation);
    if (no_memory) return NULL;
    return allocation = malloc(n);
}
void kfree(void *p)
{
    assert(p && p == allocation);
    free(p);
    allocation = NULL;
    frees++;
}
void task_sleep(uint32_t ms)
{
    assert(ms == 1 && bytes_since_nap == 64u * 1024u);
    bytes_since_nap = 0;
    naps++;
}
int task_create_stack(const char *name, void (*fn)(void *), void *arg,
                      uint8_t priority, size_t stack_size)
{
    assert(!strcmp(name, "imedict") && !arg && priority == TASK_PRIO_DEFAULT);
    assert(stack_size == 16384 && !entry);
    entry = fn;
    return (int)loader.id;
}
task_t *task_current(void) { return &loader; }
int task_stack_inspect(uint32_t id, task_stack_info_t *info)
{
    assert(id == loader.id);
    *info = (task_stack_info_t){.size = 16384, .used = 1024, .guard_ok = true};
    return 0;
}
uint64_t hal_time_now_us(void) { static uint64_t now; return now += 1000; }
void kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    size_t used = strlen(receipt);
    vsnprintf(receipt + used, sizeof receipt - used, fmt, ap);
    va_end(ap);
}
static void wr32(unsigned char *p, uint32_t n)
{
    for (unsigned i = 0; i < 4; i++) p[i] = (unsigned char)(n >> (8 * i));
}
static void reset(size_t size)
{
    ime_dict_clear();
    if (allocation) kfree(allocation);
    length = size;
    position = bytes_since_nap = reads = naps = 0;
    stat_rc = open_rc = close_rc = closes = frees = read_fault = 0;
    no_memory = false;
    type = FS_TYPE_FILE;
    receipt[0] = 0;
    entry = NULL;
    for (size_t i = 0; i < sizeof data; i++) data[i] = (unsigned char)(i * 29u);
    wr32(data, IME_DICT_MAGIC);
    wr32(data + 4, 1);
    wr32(data + 8, (uint32_t)size);
    wr32(data + 12, 16);
    data[16] = 4;
    memcpy(data + 17, "test", 4);
    data[21] = 10;
    data[22] = 0;
    memcpy(data + 23, "candidate", 10);
}
static void run(void)
{
    ime_dict_load_async();
    assert(entry);
    entry(NULL);
}
static void rejected(void)
{
    assert(!candidates() && !allocation && !strstr(receipt, "dictionary loaded"));
}
int main(void)
{
    reset(256u * 1024u + 123u);
    run();
    assert(candidates() == 1 && position == length && closes == 1 && !frees);
    assert(!memcmp(data, allocation, length) && naps == 4 && reads == 33);
    assert(strstr(receipt, "bytes=262267") && strstr(receipt, "guard OK"));
    reset(64u * 1024u);
    run();
    assert(candidates() == 1 && !naps && closes == 1);
    reset(sizeof data);
    run();
    assert(candidates() == 1 && !memcmp(data, allocation, length));
    assert(naps == 95 && closes == 1);
    reset(256u * 1024u); stat_rc = ERR_NOENT; run(); rejected(); assert(!closes);
    reset(256u * 1024u); type = FS_TYPE_DIR; run(); rejected(); assert(!closes);
    reset(IME_DICT_HEADER - 1); run(); rejected(); assert(!closes);
    reset(sizeof data + 1); run(); rejected(); assert(!closes);
    reset(256u * 1024u); open_rc = ERR_IO; run(); rejected(); assert(!closes);
    reset(256u * 1024u); no_memory = true; run(); rejected(); assert(closes == 1);
    for (int fault = 1; fault <= 4; fault++) {
        reset(256u * 1024u); read_fault = fault; run(); rejected();
        assert(closes == 1 && frees == 1 && position == 8192);
    }
    reset(256u * 1024u); close_rc = ERR_IO; run(); rejected();
    assert(closes == 1 && frees == 1);
    reset(256u * 1024u); data[0] = 0; run(); rejected();
    assert(closes == 1 && frees == 1);
    puts("IME loader PASS: exact bytes, bounded scheduling, unchanged stack, publication after close, failure cleanup");
}
