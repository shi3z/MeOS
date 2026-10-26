#include "cow.h"
#include <string.h>
/* Wire format: magic, type, base-id(lo/hi), generation, LBA, transaction
 * digest, sector count, payload[512], reserved[4], CRC32. All integers LE.
 * Data and commit records are fixed size. Never overwrite a record. */
static uint32_t get(const unsigned char *p) {
    return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static void put(unsigned char *p,uint32_t v) {
    for(unsigned i=0;i<4;i++)p[i]=(unsigned char)(v>>(8*i));
}
static uint32_t crc(const unsigned char *p,size_t n) {
    uint32_t c=~0u;
    while(n--){c^=*p++;for(int i=0;i<8;i++)c=(c>>1)^(0xedb88320u & (0u-(c&1)));}
    return ~c;
}
static uint32_t fold(uint32_t a,uint32_t b) {
    unsigned char x[8];put(x,a);put(x+4,b);return crc(x,8);
}
static int zero(const unsigned char *p,size_t n) {
    while(n--)if(*p++)return 0;
    return 1;
}
static int valid(struct cow *c,unsigned char *r) {
    return get(r)==0x31574f43 && get(r+8)==(uint32_t)c->base_id &&
        get(r+12)==(uint32_t)(c->base_id>>32) && get(r+28)==c->sectors &&
        get(r+548)==crc(r,548);
}
int cow_mount_indexed(struct cow *c,struct cow_io io,uint64_t id,uint32_t sectors,
              int (*read_base)(void *,uint32_t,void *),void *ctx,
              uint32_t *offsets,size_t index_sectors) {
    if(!c||!io.read||!io.write||!io.sync||!read_base||!sectors||io.capacity<COW_RECORD||
       (offsets&&index_sectors<sectors))return -1;
    *c=(struct cow){.log=io,.base_ctx=ctx,.base_read=read_base,.base_id=id,.sectors=sectors};
    if(offsets)for(uint32_t i=0;i<sectors;i++)offsets[i]=UINT32_MAX;
    unsigned char r[COW_RECORD];uint32_t digest=0;
    while(c->end<=io.capacity-COW_RECORD) {
        if(io.read(io.ctx,c->end,r,sizeof r))return -1;
        if(zero(r,sizeof r))break;
        if(get(r)==0x31574f43 && get(r+548)==crc(r,548) &&
           (get(r+8)!=(uint32_t)id || get(r+12)!=(uint32_t)(id>>32) || get(r+28)!=sectors))return -1;
        if(!valid(c,r)) {c->sealed=1;break;}
        if(c->generation==UINT32_MAX||get(r+16)!=c->generation+1){c->sealed=1;break;}
        if(get(r+4)==1 && get(r+20)<sectors){
            digest=fold(digest,get(r+548));
            if(offsets)offsets[get(r+20)]=c->end;
        }
        else if(get(r+4)==2 && get(r+24)==digest) {
            c->generation++;c->committed=c->end+COW_RECORD;digest=0;
        } else {c->sealed=1;break;}
        c->end+=COW_RECORD;
    }
    /* A partial transaction requires compaction into a fresh log. */
    if(c->end!=c->committed){
        c->sealed=1;
        /* Tentative records may have replaced committed offsets. This is the
         * exceptional recovery path; rebuild only through the last commit. */
        if(offsets&&cow_index(c,offsets,index_sectors))return -1;
    } else c->index=offsets;
    return 0;
}
int cow_mount(struct cow *c,struct cow_io io,uint64_t id,uint32_t sectors,
              int (*read_base)(void *,uint32_t,void *),void *ctx) {
    return cow_mount_indexed(c,io,id,sectors,read_base,ctx,NULL,0);
}
int cow_index(struct cow *c,uint32_t *offsets,size_t sectors) {
    if(!c||!offsets||sectors<c->sectors||c->active)return -1;
    c->index=NULL;
    for(uint32_t i=0;i<c->sectors;i++)offsets[i]=UINT32_MAX;
    unsigned char r[COW_RECORD];
    for(uint32_t p=0;p<c->committed;p+=COW_RECORD) {
        if(c->log.read(c->log.ctx,p,r,sizeof r)||!valid(c,r))return -1;
        if(get(r+4)==1){if(get(r+20)>=c->sectors)return -1;offsets[get(r+20)]=p;}
    }
    c->index=offsets;return 0;
}
int cow_read(struct cow *c,uint32_t lba,void *out) {
    if(!c||!out||lba>=c->sectors)return -1;
    unsigned char r[COW_RECORD];
    if(c->index) {
        uint32_t p=c->index[lba];
        if(p==UINT32_MAX)return c->base_read(c->base_ctx,lba,out);
        if(c->log.read(c->log.ctx,p,r,sizeof r)||!valid(c,r)||get(r+4)!=1||get(r+20)!=lba)return -1;
        memcpy(out,r+32,COW_SECTOR);return 0;
    }
    if(c->base_read(c->base_ctx,lba,out))return -1;
    /* Linear replay is intentionally a correctness baseline, not a fast index.
     * Pending writes are visible to the live M5FS transaction only. */
    uint32_t limit=c->active?c->end:c->committed;
    for(uint32_t pos=0;pos<limit;pos+=COW_RECORD) {
        if(c->log.read(c->log.ctx,pos,r,sizeof r)||!valid(c,r))return -1;
        if(get(r+4)==1&&get(r+20)==lba)memcpy(out,r+32,COW_SECTOR);
    }
    return 0;
}
int cow_begin(struct cow *c) {
    if(!c||c->sealed||c->active||c->generation==UINT32_MAX)return -1;
    c->active=1;c->digest=0;return 0;
}
static int append(struct cow *c,uint32_t type,uint32_t lba,const void *data) {
    if(c->end>c->log.capacity-COW_RECORD){c->sealed=1;c->active=0;c->index=NULL;return -1;}
    unsigned char r[COW_RECORD]={0};
    put(r,0x31574f43);put(r+4,type);put(r+8,(uint32_t)c->base_id);
    put(r+12,(uint32_t)(c->base_id>>32));put(r+16,c->generation+1);
    put(r+20,lba);put(r+24,c->digest);put(r+28,c->sectors);
    if(data)memcpy(r+32,data,COW_SECTOR);
    put(r+548,crc(r,548));
    if(c->log.write(c->log.ctx,c->end,r,sizeof r)){c->sealed=1;c->active=0;c->index=NULL;return -1;}
    c->end+=COW_RECORD;
    if(type==1)c->digest=fold(c->digest,get(r+548));
    return 0;
}
int cow_write(struct cow *c,uint32_t lba,const void *data) {
    if(!c||!c->active||c->sealed||!data||lba>=c->sectors)return -1;
    /* Reserve a commit record before accepting another sector. */
    if(c->log.capacity-c->end<2*COW_RECORD){c->sealed=1;c->active=0;c->index=NULL;return -1;}
    uint32_t pos=c->end;
    int rc=append(c,1,lba,data);
    if(!rc&&c->index)c->index[lba]=pos;
    return rc;
}
int cow_commit(struct cow *c) {
    if(!c||!c->active||c->sealed)return -1;
    if(c->log.sync(c->log.ctx)||append(c,2,0,0)||c->log.sync(c->log.ctx)) {
        c->sealed=1;c->active=0;c->index=NULL;return -1;
    }
    c->committed=c->end;c->generation++;c->active=0;return 0;
}
