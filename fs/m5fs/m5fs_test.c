/*
 * M5FS self test.  Runs entirely on a RAM disk: it never touches real media.
 */
#include <m5os/fs.h>
#include <m5os/errno.h>
#include <m5os/kmalloc.h>
#include <m5os/kprintf.h>
#include "../../drivers/ramdisk/ramdisk.h"

#define RD_BLOCKS      8192u                 /* 4 MiB volume */
#define BIG_SIZE       100000u
#define SMALL_FILES    40
#define SMALL_BLOCKS   2u
#define IO_CHUNK       777u                  /* deliberately not a divisor of 512 */

#define CHECK(cond) do { if (!(cond)) { kprintf("  FAIL line %d: %s\n", __LINE__, #cond); ok = false; goto done; } } while (0)

static uint8_t pat(uint32_t seed, uint32_t i) { return (uint8_t)(seed + i * 7u + (i >> 8)); }

static int write_pattern(const char *path, uint32_t size, uint32_t seed, uint32_t flags)
{
    int fd = fs_open(path, FS_O_WRITE | flags);
    if (fd < 0) return fd;
    static uint8_t buf[IO_CHUNK];
    uint32_t done = 0;
    int rc = 0;
    if (flags & FS_O_APPEND) { fs_seek(fd, 0, FS_SEEK_END); done = 0; }
    uint32_t base = (flags & FS_O_APPEND) ? 0 : 0;
    (void)base;
    while (done < size) {
        uint32_t n = size - done < IO_CHUNK ? size - done : IO_CHUNK;
        for (uint32_t i = 0; i < n; i++) buf[i] = pat(seed, done + i);
        rc = fs_write(fd, buf, n);
        if (rc <= 0) break;
        done += (uint32_t)rc;
    }
    fs_close(fd);
    return done == size ? 0 : (rc < 0 ? rc : ERR_NOSPC);
}

static bool verify_pattern(const char *path, uint32_t size, uint32_t seed)
{
    int fd = fs_open(path, FS_O_READ);
    if (fd < 0) return false;
    static uint8_t buf[IO_CHUNK];
    uint32_t done = 0;
    bool good = true;
    while (good) {
        int n = fs_read(fd, buf, IO_CHUNK);
        if (n < 0) { good = false; break; }
        if (n == 0) break;
        for (int i = 0; i < n; i++) if (buf[i] != pat(seed, done + (uint32_t)i)) { good = false; break; }
        done += (uint32_t)n;
    }
    fs_close(fd);
    return good && done == size;
}

static int count_cb(const fs_info_t *info, void *ctx)
{
    (void)info;
    (*(int *)ctx)++;
    return 0;
}

static uint32_t free_blocks(void)
{
    fs_stat_t st;
    fs_statfs(&st);
    return st.free_blocks;
}

bool fs_selftest(void)
{
    bool ok = true;
    kmem_stats_t before, after;
    kmem_psram_stats(&before);

    blockdev_t *rd = ramdisk_create(RD_BLOCKS);
    if (!rd) { kprintf("fs selftest: cannot create RAM disk\n"); return false; }

    CHECK(fs_format(rd, 0, RD_BLOCKS) == ERR_OK);
    CHECK(fs_mount(rd, 0) == ERR_OK);
    uint32_t free0 = free_blocks();

    /* --- layout & directories ------------------------------------------- */
    int n = 0;
    CHECK(fs_list("/", count_cb, &n) == 3 && n == 3);                 /* system, apps, home */
    fs_info_t info;
    CHECK(fs_stat("/home", &info) == ERR_OK && info.type == FS_TYPE_DIR);
    CHECK(fs_mkdir("/home/user") == ERR_OK);
    CHECK(fs_mkdir("/home/user") == ERR_EXIST);
    CHECK(fs_mkdir("/nope/x") == ERR_NOENT);
    CHECK(fs_mkdir("/home/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") == ERR_NAMETOOLONG);
    CHECK(fs_open("/home/missing.txt", FS_O_READ) == ERR_NOENT);
    CHECK(fs_open("/home", FS_O_READ) == ERR_ISDIR);
    CHECK(fs_open("/home/user/x/y", FS_O_READ) == ERR_NOENT);

    /* --- small file round trip ------------------------------------------ */
    static const char msg[] = "Hello, M5FS!\n";
    int fd = fs_open("/home/hello.txt", FS_O_WRITE | FS_O_CREATE);
    CHECK(fd >= 0);
    CHECK(fs_write(fd, msg, sizeof msg - 1) == (int)sizeof msg - 1);
    CHECK(fs_close(fd) == ERR_OK);
    char rb[32];
    fd = fs_open("/home/hello.txt", FS_O_READ);
    CHECK(fd >= 0);
    CHECK(fs_read(fd, rb, sizeof rb) == (int)sizeof msg - 1);
    for (unsigned i = 0; i < sizeof msg - 1; i++) CHECK(rb[i] == msg[i]);
    CHECK(fs_read(fd, rb, sizeof rb) == 0);                           /* EOF */
    fs_close(fd);
    CHECK(fs_stat("/home/hello.txt", &info) == ERR_OK && info.size == sizeof msg - 1);

    /* --- big file, odd chunk sizes (partial blocks, RMW) ---------------- */
    CHECK(write_pattern("/home/big.bin", BIG_SIZE, 11, FS_O_CREATE) == 0);
    CHECK(verify_pattern("/home/big.bin", BIG_SIZE, 11));

    /* --- overwrite in the middle via seek (read-modify-write) ------------ */
    fd = fs_open("/home/big.bin", FS_O_READ | FS_O_WRITE);
    CHECK(fd >= 0);
    CHECK(fs_seek(fd, 1000, FS_SEEK_SET) == 1000);
    uint8_t patch[5] = { 1, 2, 3, 4, 5 };
    CHECK(fs_write(fd, patch, 5) == 5);
    CHECK(fs_seek(fd, 998, FS_SEEK_SET) == 998);
    uint8_t chk[9];
    CHECK(fs_read(fd, chk, 9) == 9);
    CHECK(chk[0] == pat(11, 998) && chk[1] == pat(11, 999) && chk[2] == 1 && chk[6] == 5 && chk[7] == pat(11, 1005));
    CHECK(fs_seek(fd, BIG_SIZE + 1, FS_SEEK_SET) == ERR_INVAL);       /* no seeking past EOF */
    fs_close(fd);

    /* --- append ----------------------------------------------------------- */
    fd = fs_open("/home/hello.txt", FS_O_WRITE | FS_O_APPEND);
    CHECK(fd >= 0 && fs_write(fd, "more", 4) == 4);
    fs_close(fd);
    CHECK(fs_stat("/home/hello.txt", &info) == ERR_OK && info.size == sizeof msg - 1 + 4);

    /* --- persistence: unmount, mount, re-read ----------------------------- */
    fd = fs_open("/home/big.bin", FS_O_READ);
    CHECK(fd >= 0);                  /* leaves an fd open */
    CHECK(fs_unmount() == ERR_BUSY);                                  /* ... so unmount must refuse */
    CHECK(fs_close(fd) == ERR_OK);
    CHECK(fs_unmount() == ERR_OK);
    CHECK(fs_mount(rd, 0) == ERR_OK);
    CHECK(fs_stat("/home/user", &info) == ERR_OK && info.type == FS_TYPE_DIR);
    CHECK(fs_stat("/home/big.bin", &info) == ERR_OK && info.size == BIG_SIZE);
    fd = fs_open("/home/hello.txt", FS_O_READ);
    CHECK(fd >= 0 && fs_read(fd, rb, sizeof rb) == (int)sizeof msg - 1 + 4 && rb[sizeof msg - 1] == 'm');
    fs_close(fd);

    /* --- delete frees space; non-empty directory is protected ------------- */
    CHECK(fs_delete("/home") == ERR_NOTEMPTY);
    CHECK(fs_delete("/home/big.bin") == ERR_OK);
    CHECK(fs_delete("/home/hello.txt") == ERR_OK);
    CHECK(fs_delete("/home/user") == ERR_OK);
    CHECK(fs_delete("/home/hello.txt") == ERR_NOENT);
    CHECK(free_blocks() == free0);

    /* --- fragmentation, extent limit, full disk --------------------------- */
    char name[24];
    for (int i = 0; i < SMALL_FILES; i++) {
        name[0] = '/'; name[1] = 'a'; name[2] = (char)('0' + i / 10); name[3] = (char)('0' + i % 10); name[4] = 0;
        CHECK(write_pattern(name, SMALL_BLOCKS * 512, 100 + (uint32_t)i, FS_O_CREATE) == 0);
    }
    fd = fs_open("/pad", FS_O_WRITE | FS_O_CREATE);                   /* fill the rest of the disk */
    CHECK(fd >= 0);
    static uint8_t chunk[4096];
    int wr;
    while ((wr = fs_write(fd, chunk, sizeof chunk)) > 0) {}
    CHECK(wr == ERR_NOSPC && free_blocks() == 0);
    fs_close(fd);
    for (int i = 0; i < SMALL_FILES; i += 2) {                        /* punch 2-block holes */
        name[0] = '/'; name[1] = 'a'; name[2] = (char)('0' + i / 10); name[3] = (char)('0' + i % 10); name[4] = 0;
        CHECK(fs_delete(name) == ERR_OK);
    }
    CHECK(write_pattern("/frag", 7 * 512, 77, FS_O_CREATE) == 0);     /* needs 4 extents of 2 blocks */
    CHECK(verify_pattern("/frag", 7 * 512, 77));
    fd = fs_open("/frag2", FS_O_WRITE | FS_O_CREATE);
    int w1 = fs_write(fd, chunk, 10 * 512);                           /* needs >4 extents: must come back short */
    CHECK(w1 > 0 && w1 < 10 * 512);
    CHECK(fs_write(fd, chunk, 512) == ERR_NOSPC);
    fs_close(fd);
    for (int i = 1; i < SMALL_FILES; i += 2) {                        /* survivors are intact */
        name[0] = '/'; name[1] = 'a'; name[2] = (char)('0' + i / 10); name[3] = (char)('0' + i % 10); name[4] = 0;
        CHECK(verify_pattern(name, SMALL_BLOCKS * 512, 100 + (uint32_t)i));
        CHECK(fs_delete(name) == ERR_OK);
    }
    CHECK(fs_delete("/pad") == ERR_OK && fs_delete("/frag") == ERR_OK && fs_delete("/frag2") == ERR_OK);
    CHECK(free_blocks() == free0);                                    /* nothing leaked */

    /* --- corruption is detected ------------------------------------------- */
    CHECK(fs_unmount() == ERR_OK);
    uint8_t junk[512];
    for (int i = 0; i < 512; i++) junk[i] = (uint8_t)(i * 31);
    CHECK(rd->write(rd, 0, 1, junk) == ERR_OK);
    CHECK(fs_mount(rd, 0) == ERR_CORRUPT);

done:
    ramdisk_destroy(rd);
    kmem_psram_stats(&after);
    if (after.used != before.used) { kprintf("  FAIL: PSRAM leak %u -> %u\n", (uint32_t)before.used, (uint32_t)after.used); ok = false; }
    kprintf("fs selftest (M5FS on RAM disk): %s\n", ok ? "PASS" : "FAIL");
    return ok;
}
