#include <m5os/board_config.h>
#if M5OS_QUAKE
#include <m5os/vfs.h>
#include <m5os/crc32.h>
#include <m5os/task.h>
#include <m5os/kprintf.h>
int quake_verify(void) {
    extern const char *quake_storage_base(void);
    char path[96];ksnprintf(path,sizeof path,"%s/id1/pak0.pak",quake_storage_base());
    int fd=vfs_open(path,VFS_O_READ);
    if(fd<0){kprintf("QVERIFY failed open %d\n",fd);return 1;}
    uint8_t block[4096];uint32_t size=0,sum=0;int n;
    while((n=vfs_read(fd,block,sizeof block))>0){sum=crc32_update(sum,block,(size_t)n);size+=(uint32_t)n;task_yield();}
    int rc=vfs_close(fd);
    if(n<0||rc<0){kputs("QVERIFY read/close failure\n");return 1;}
    kprintf("QVERIFY %u %08x\n",size,sum);return 0;
}
#endif
