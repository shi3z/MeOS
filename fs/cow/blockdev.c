#include "blockdev.h"
#include <m5os/errno.h>
static int read_blocks(blockdev_t *bd,uint32_t lba,uint32_t n,void *buf) {
    struct cow *c=bd->priv;
    if(lba>c->sectors||n>c->sectors-lba)return ERR_INVAL;
    for(uint32_t i=0;i<n;i++)if(cow_read(c,lba+i,(unsigned char *)buf+(size_t)i*512))return ERR_IO;
    return ERR_OK;
}
static int write_blocks(blockdev_t *bd,uint32_t lba,uint32_t n,const void *buf) {
    struct cow *c=bd->priv;
    if(lba>c->sectors||n>c->sectors-lba)return ERR_INVAL;
    for(uint32_t i=0;i<n;i++)if(cow_write(c,lba+i,(const unsigned char *)buf+(size_t)i*512))return ERR_IO;
    return ERR_OK;
}
void cow_blockdev_init(blockdev_t *bd,struct cow *c) {
    *bd=(blockdev_t){.name="cow",.block_size=512,.block_count=c->sectors,
        .read=read_blocks,.write=write_blocks,.priv=c};
}
