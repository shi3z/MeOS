/* The large SKK dictionary is an SD bulk asset.  It is copied to PSRAM by a
 * background task after /assets has mounted; until then (or without SD) the
 * built-in mini dictionary serves. */
#include <m5os/kprintf.h>
#include <m5os/kmalloc.h>
#include <m5os/task.h>
#include <m5os/vfs.h>
#include <m5os/fs.h>
#include "ime_dict.h"

#define DICT_PATH        "/assets/ime-dict.bin"
#define DICT_MAX         (6u * 1024u * 1024u)
#define CHUNK            2048u
#define CHUNKS_PER_NAP   4
#define LOADER_STACK     16384u

static void loader_task(void *arg)
{
    (void)arg;
    vfs_info_t info;
    if (vfs_stat(DICT_PATH, &info) != 0 || info.type != FS_TYPE_FILE) {
        kprintf("ime: no SD dictionary (%s), using the built-in one\n", DICT_PATH);
        return;
    }
    if (info.size < IME_DICT_HEADER || info.size > DICT_MAX) {
        kprintf("ime: SD dictionary has invalid size (%u bytes)\n", (unsigned)info.size);
        return;
    }
    int fd = vfs_open(DICT_PATH, VFS_O_READ);
    if (fd < 0) { kprintf("ime: cannot open SD dictionary (%d)\n", fd); return; }
    uint8_t *buf = kmalloc_psram(info.size);
    if (!buf) {
        kprintf("ime: no PSRAM for the dictionary (%u bytes)\n", (unsigned)info.size);
        vfs_close(fd);
        return;
    }
    uint32_t off = 0;
    while (off < info.size) {
        uint32_t n = info.size - off < CHUNK ? info.size - off : CHUNK;
        int got = vfs_read(fd, buf + off, n);
        if (got != (int)n) {
            kprintf("ime: SD dictionary read failed at %u (%d)\n", (unsigned)off, got);
            vfs_close(fd); kfree(buf); return;
        }
        off += n;
        if ((off / CHUNK) % CHUNKS_PER_NAP == 0) task_sleep(1);
    }
    vfs_close(fd);
    if (ime_dict_add(buf, info.size)) {
        task_stack_info_t stack;
        kprintf("ime: SD dictionary loaded (%u KB)\n", (unsigned)(info.size / 1024u));
        if (task_stack_inspect(task_current()->id, &stack) == 0)
            kprintf("ime: loader stack %u/%u bytes, guard %s\n", (unsigned)stack.used,
                    (unsigned)stack.size, stack.guard_ok ? "OK" : "FAILED");
    } else { kprintf("ime: SD dictionary is damaged\n"); kfree(buf); }
}

void ime_dict_load_async(void)
{
    task_create_stack("imedict", loader_task, 0, TASK_PRIO_DEFAULT, LOADER_STACK);
}
