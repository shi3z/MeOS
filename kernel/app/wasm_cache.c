#include "wasm_cache.h"
#include <m5os/vfs.h>
#include <m5os/errno.h>
#include <string.h>
/* An identical persistent cache never needs truncation or writes at boot. */
bool wasm_cache_matches(const char *path,const uint8_t *data,uint32_t size)
{
    if(!data&&size)return false;
    vfs_info_t st;
    if(vfs_stat(path,&st)!=ERR_OK || st.type!=FS_TYPE_FILE || st.size!=size)return false;
    int fd=vfs_open(path,VFS_O_READ);if(fd<0)return false;
    uint8_t bytes[512];uint32_t off=0;bool same=true;
    while(off<size){
        size_t n=size-off;if(n>sizeof bytes)n=sizeof bytes;
        int got=vfs_read(fd,bytes,n);
        if(got<=0 || (size_t)got>n || memcmp(bytes,data+off,(size_t)got)){same=false;break;}
        off+=(uint32_t)got;
    }
    if(vfs_close(fd)<0)same=false;
    return same;
}
