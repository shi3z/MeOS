#include <m5os/vfs.h>
#include <m5os/errno.h>
/* Legacy filesystem/host fallback. Called only when no replace hook exists,
 * never after a transactional handler reports an error. */
int vfs_write_file(const char *path,const void *data,size_t len,size_t *written)
{
    size_t done=0;
    if(written)*written=0;
    if((!data&&len)||len>0x7fffffffu)return ERR_INVAL;
    int fd=vfs_open(path,VFS_O_WRITE|VFS_O_CREATE|VFS_O_TRUNC);
    if(fd<0)return fd;
    int rc=ERR_OK;
    while(done<len){
        int n=vfs_write(fd,(const uint8_t *)data+done,len-done);
        if(n<=0){rc=n<0?n:ERR_NOSPC;break;}
        if((size_t)n>len-done){rc=ERR_IO;break;}
        done+=(size_t)n;
    }
    int closed=vfs_close(fd);
    if(rc==ERR_OK&&closed<0)rc=closed;
    if(written)*written=done;
    return rc;
}
