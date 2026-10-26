#include "../../fs/cow/blockdev.h"
#include <m5os/errno.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned char log_bytes[8*COW_RECORD];
static unsigned reads,writes,syncs;
static int fail_read,fail_write;
static int read_log(void *ctx,uint32_t off,void *out,size_t n) {
    (void)ctx;assert(off<=sizeof log_bytes&&n<=sizeof log_bytes-off);reads++;
    if(fail_read)return -1;
    memcpy(out,log_bytes+off,n);return 0;
}
static int write_log(void *ctx,uint32_t off,const void *in,size_t n) {
    (void)ctx;assert(off<=sizeof log_bytes&&n<=sizeof log_bytes-off);writes++;
    if(fail_write)return -1;
    memcpy(log_bytes+off,in,n);return 0;
}
static int sync_log(void *ctx){(void)ctx;syncs++;return 0;}
static int read_base(void *ctx,uint32_t lba,void *out){(void)ctx;assert(lba<4);memset(out,(int)lba,512);return 0;}
int main(void) {
    struct cow c;struct cow_io io={NULL,sizeof log_bytes,read_log,write_log,sync_log};
    assert(!cow_mount(&c,io,42,4,read_base,NULL));
    blockdev_t bd;cow_blockdev_init(&bd,&c);
    assert(bd.block_size==512&&bd.block_count==4&&bd.priv==&c);
    unsigned char data[1024];assert(!bd.read(&bd,1,2,data));
    for(unsigned i=0;i<sizeof data;i++)assert(data[i]==(i<512?1:2));
    unsigned before=reads;
    assert(bd.read(&bd,4,1,data)==ERR_INVAL);
    assert(bd.read(&bd,UINT32_MAX,2,data)==ERR_INVAL);
    assert(bd.write(&bd,3,2,data)==ERR_INVAL&&reads==before&&!writes&&!syncs);
    assert(bd.write(&bd,1,1,data)==ERR_IO&&!writes&&!syncs); /* no implicit begin */
    assert(!cow_begin(&c));memset(data,77,sizeof data);
    assert(!bd.write(&bd,1,2,data)&&writes==2&&!syncs&&c.active&&!c.committed);
    memset(data,0,sizeof data);assert(!bd.read(&bd,1,2,data));
    for(unsigned i=0;i<sizeof data;i++)assert(data[i]==77);
    assert(!cow_commit(&c)&&syncs==2);
    fail_read=1;assert(bd.read(&bd,1,1,data)==ERR_IO);fail_read=0;
    assert(!cow_begin(&c));fail_write=1;
    assert(bd.write(&bd,1,1,data)==ERR_IO&&c.sealed&&!c.active);
    assert(cow_commit(&c)<0);fail_write=0;
    assert(!cow_mount(&c,io,42,4,read_base,NULL));
    assert(!bd.read(&bd,1,2,data));
    for(unsigned i=0;i<sizeof data;i++)assert(data[i]==77);
    puts("COW blockdev: sector ranges, pending reads, explicit transactions and IO errors PASS");
}
