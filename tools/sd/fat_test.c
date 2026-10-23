#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <m5os/fat.h>
#include <m5os/errno.h>
#include <m5os/mutex.h>
#include <m5os/app.h>
#include "ff.h"
#include "diskio.h"

static uint8_t *image;
static size_t image_size;
static bool permitted = true, fail_reads;
static int depth, writes, entries;
static const vfs_fs_t *fs;
void mutex_lock(mutex_t *m) { (void)m; assert(!depth); depth++; }
void mutex_unlock(mutex_t *m) { (void)m; assert(depth == 1); depth--; }
void *kmalloc(size_t n) { return malloc(n); }
void kfree(void *p) { free(p); }
bool task_has_perm(uint32_t p) { assert(p == APP_PERM_FS_READ); return permitted; }
int vfs_mount(const char *path, const vfs_fs_t *v, void *data)
{
    (void)data; assert(!strcmp(path, "/sd") && !fs); fs = v; return ERR_OK;
}
int vfs_umount(const char *path) { assert(!strcmp(path, "/sd") && fs); fs = NULL; return ERR_OK; }
static int read_blocks(blockdev_t *bd, uint32_t lba, uint32_t count, void *buf)
{
    assert(depth == 1 && count && lba < bd->block_count && count <= bd->block_count - lba);
    if (fail_reads) return ERR_IO;
    memcpy(buf, image + (size_t)lba * 512, (size_t)count * 512); return ERR_OK;
}
static int write_blocks(blockdev_t *bd, uint32_t lba, uint32_t count, const void *buf)
{
    (void)bd; (void)lba; (void)count; (void)buf; writes++; return ERR_IO;
}
static int listing(const vfs_info_t *i, void *ctx)
{
    (void)ctx; assert(!depth); entries++;
    vfs_info_t info;
    assert(fs->stat(NULL, "/HELLO.TXT", &info) == ERR_OK);
    assert(fat_unmount() == ERR_BUSY); /* Callback can re-enter: no lock deadlock or unmount. */
    assert(i->name[0] && strlen(i->name)<=VFS_NAME_MAX);
    return 0;
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    FILE *f = fopen(argv[1], "rb"); assert(f);
    assert(!fseek(f, 0, SEEK_END)); image_size = (size_t)ftell(f); rewind(f);
    image = malloc(image_size); assert(image && fread(image, 1, image_size, f) == image_size); fclose(f);
    blockdev_t bd = { .block_size = 512, .block_count = (uint32_t)(image_size / 512), .read = read_blocks, .write = write_blocks };
    assert(fat_mount_readonly(&bd) == ERR_OK && fat_is_mounted());
    assert(fat_mount_readonly(&bd) == ERR_BUSY);
    vfs_info_t info;
    assert(fs->stat(NULL, "/", &info) == ERR_OK && info.type == FS_TYPE_DIR);
    assert(fs->stat(NULL, "/hello.txt", &info) == ERR_OK && info.size == 1300);
    assert(fs->stat(NULL,"/長い日本語の録音ファイル名 2026年10月24日.mp3",&info)==ERR_OK && info.size==1300);
    assert(fs->stat(NULL,"/RECORD~1.MP3",&info)==ERR_OK);
    assert(fs->stat(NULL,"/音楽🎵.mp3",&info)==ERR_OK);
    char longpath[VFS_PATH_MAX];longpath[0]='/';
    memset(longpath+1,'a',251);memcpy(longpath+252,".mp3",5);
    assert(fs->stat(NULL,longpath,&info)==ERR_OK);
    for(unsigned i=0;i<251;i++)memcpy(longpath+1+i*3,"録",3);
    memcpy(longpath+754,".mp3",5);assert(fs->stat(NULL,longpath,&info)==ERR_OK);
    assert(fs->stat(NULL, "/missing", &info) == ERR_NOENT);
    void *file = NULL;
    assert(fs->open(NULL, "/HELLO.TXT", VFS_O_READ | VFS_O_WRITE, &file) == ERR_PERM);
    assert(fs->mkdir(NULL, "/NEW") == ERR_PERM && fs->unlink(NULL, "/HELLO.TXT") == ERR_PERM);
    permitted = false;
    assert(fs->open(NULL, "/HELLO.TXT", VFS_O_READ, &file) == ERR_PERM);
    assert(fs->stat(NULL, "/HELLO.TXT", &info) == ERR_PERM);
    assert(fs->readdir(NULL, "/", listing, NULL) == ERR_PERM);
    permitted = true;
    assert(fs->open(NULL, "/HELLO.TXT", VFS_O_READ, &file) == ERR_OK);
    assert(fat_unmount() == ERR_BUSY);
    uint8_t buf[1400];
    assert(fs->read(file, buf, sizeof buf) == 1300);
    for (unsigned i = 0; i < 1300; i++) assert(buf[i] == i % 251);
    assert(fs->read(file, buf, sizeof buf) == 0);
    assert(fs->seek(file, -10, FS_SEEK_END) == 1290);
    assert(fs->read(file, buf, 20) == 10);
    for (unsigned i = 0; i < 10; i++) assert(buf[i] == (1290 + i) % 251);
    assert(fs->seek(file, -2000, FS_SEEK_CUR) == ERR_INVAL);
    assert(fs->write(file, buf, 1) == ERR_PERM);
    assert(fs->close(file) == ERR_OK);
    assert(fs->readdir(NULL, "/", listing, NULL) == 6 && entries == 6);
    if (strstr(argv[1], "cycle")) {
        assert(fs->readdir(NULL, "/DIR", NULL, NULL) < 0);
    } else {
        assert(fs->open(NULL, "/DIR/NEST.TXT", VFS_O_READ, &file) == ERR_OK);
        assert(fs->read(file, buf, sizeof buf) == 7 && !memcmp(buf, "nested\n", 7));
        assert(fs->close(file) == ERR_OK);
    }
    assert(disk_write(0, buf, 0, 1) == RES_WRPRT && writes == 0);
    assert(fat_unmount() == ERR_OK && !fat_is_mounted());
    fail_reads = true; assert(fat_mount_readonly(&bd) < 0 && !fat_is_mounted() && !fs);
    fail_reads = false;
    bd.block_count = 1; assert(fat_mount_readonly(&bd) < 0 && !fat_is_mounted());
    assert(writes == 0 && !depth);
    free(image);
    printf("FAT read-only, fragmentation, permissions, nested paths, bounds/error tests PASS: %s\n", argv[1]);
}
