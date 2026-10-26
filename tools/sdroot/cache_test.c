#include "../../kernel/app/wasm_cache.h"
#include <m5os/vfs.h>
#include <m5os/errno.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
static uint8_t stored[1700],expected[1700];
static unsigned position,opens,closes,short_read;
static int stat_error,open_error,read_error,close_error;
int vfs_stat(const char *p,vfs_info_t *s){(void)p;*s=(vfs_info_t){.type=FS_TYPE_FILE,.size=sizeof stored};return stat_error;}
int vfs_open(const char *p,uint32_t flags){(void)p;assert(flags==VFS_O_READ);opens++;position=0;return open_error?open_error:4;}
int vfs_read(int fd,void *p,size_t n){
    assert(fd==4);
    if(read_error)return read_error;
    if(short_read&&n>short_read)n=short_read;
    if(n>sizeof stored-position)n=sizeof stored-position;
    memcpy(p,stored+position,n);position+=(unsigned)n;return (int)n;
}
int vfs_close(int fd){assert(fd==4);closes++;return close_error;}
int main(void){
    for(unsigned i=0;i<sizeof expected;i++)stored[i]=expected[i]=(uint8_t)i;
    assert(wasm_cache_matches("/apps/a.wasm",expected,sizeof expected));assert(opens==closes);
    short_read=7;assert(wasm_cache_matches("/apps/a.wasm",expected,sizeof expected));
    stored[900]^=1;assert(!wasm_cache_matches("/apps/a.wasm",expected,sizeof expected));stored[900]^=1;
    read_error=ERR_IO;assert(!wasm_cache_matches("/apps/a.wasm",expected,sizeof expected));read_error=0;
    close_error=ERR_IO;assert(!wasm_cache_matches("/apps/a.wasm",expected,sizeof expected));close_error=0;
    assert(opens==closes);stat_error=ERR_NOENT;assert(!wasm_cache_matches("/apps/a.wasm",expected,sizeof expected));stat_error=0;
    assert(!wasm_cache_matches("/apps/a.wasm",expected,sizeof expected-1));
    puts("Persistent WASM cache: identical/short reads, mismatch, errors, no write API PASS");
}
