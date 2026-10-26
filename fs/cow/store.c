#include "store.h"
#include <string.h>
#define HEADER 512u
static uint32_t get(const unsigned char *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void put(unsigned char *p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=(unsigned char)(v>>(8*i));}
static uint32_t crc(const unsigned char *p,size_t n){uint32_t c=~0u;while(n--){c^=*p++;for(int i=0;i<8;i++)c=(c>>1)^(0xedb88320u&(0u-(c&1)));}return ~c;}
static uint64_t get64(const unsigned char *p){return get(p)|((uint64_t)get(p+4)<<32);}
static void put64(unsigned char *p,uint64_t v){put(p,(uint32_t)v);put(p+4,(uint32_t)(v>>32));}
static int read_log(void *ctx,uint32_t o,void *p,size_t n){struct cow_bank *b=ctx;return b->io.read(b->io.ctx,HEADER+o,p,n);}
static int write_log(void *ctx,uint32_t o,const void *p,size_t n){struct cow_bank *b=ctx;return b->io.write(b->io.ctx,HEADER+o,p,n);}
static int sync_log(void *ctx){struct cow_bank *b=ctx;return b->io.sync(b->io.ctx);}
static struct cow_io log_io(struct cow_bank *b){return (struct cow_io){b,b->io.capacity-HEADER,read_log,write_log,sync_log};}
static int config(struct cow_store *s,const struct cow_bank b[2],uint64_t id,uint32_t sectors,int (*read_base)(void *,uint32_t,void *),void *ctx){
    if(!s||!b||!sectors||!read_base)return -1;
    for(int i=0;i<2;i++)if(!b[i].io.read||!b[i].io.write||!b[i].io.sync||!b[i].reset||b[i].io.capacity<HEADER+2*COW_RECORD)return -1;
    if(b[0].io.ctx==b[1].io.ctx)return -1; /* separate handles required */
    struct cow_bank copy[2]={b[0],b[1]};
    *s=(struct cow_store){.base_id=id,.sectors=sectors,.reserve_records=2,.selected=-1,.faulted=1,.base_read=read_base,.base_ctx=ctx};
    s->bank[0]=copy[0];s->bank[1]=copy[1];return 0;
}
static int header(struct cow_store *s,int i,uint64_t epoch){
    unsigned char h[HEADER]={0};put(h,0x31424f43);put(h+4,1);put64(h+8,s->base_id);
    put(h+16,s->sectors);put64(h+24,epoch);put(h+508,crc(h,508));
    return s->bank[i].io.write(s->bank[i].io.ctx,0,h,sizeof h);
}
int cow_store_recover_indexed(struct cow_store *s,uint32_t *index,size_t index_sectors){
    if(!s)return -1;
    s->faulted=1;int order[2],count=0;uint64_t epochs[2]={0,0};struct cow winner;
    for(int i=0;i<2;i++){
        unsigned char h[HEADER];
        if(s->bank[i].io.read(s->bank[i].io.ctx,0,h,sizeof h))return -1;
        if(get(h)!=0x31424f43||get(h+4)!=1||get(h+508)!=crc(h,508))continue;
        if(get64(h+8)!=s->base_id||get(h+16)!=s->sectors)return -1;
        epochs[i]=get64(h+24);if(!epochs[i])return -1;
        order[count++]=i;
    }
    if(count==2){
        if(epochs[0]==epochs[1])return -1;
        if(epochs[order[0]]<epochs[order[1]]){int t=order[0];order[0]=order[1];order[1]=t;}
    }
    /* The bank header and its first durable transaction publish an epoch.
     * Scan the newest candidate first; the older bank is recovery media and
     * does not belong on the normal boot path. */
    for(int n=0;n<count;n++){
        int i=order[n];struct cow candidate;
        if(cow_mount_indexed(&candidate,log_io(&s->bank[i]),s->base_id,s->sectors,
                             s->base_read,s->base_ctx,index,index_sectors))return -1;
        if(!candidate.generation)continue;
        winner=candidate;s->view=winner;s->selected=i;s->epoch=epochs[i];s->faulted=0;return 0;
    }
    return -1;
}
int cow_store_recover(struct cow_store *s){
    if(!s)return -1;
    return cow_store_recover_indexed(s,s->view.index,s->view.index?s->sectors:0);
}
int cow_store_open(struct cow_store *s,const struct cow_bank b[2],uint64_t id,uint32_t n,int (*r)(void *,uint32_t,void *),void *ctx){
    if(config(s,b,id,n,r,ctx))return -1;
    return cow_store_recover(s);
}
int cow_store_open_indexed(struct cow_store *s,const struct cow_bank b[2],uint64_t id,uint32_t n,int (*r)(void *,uint32_t,void *),void *ctx,uint32_t *index,size_t index_sectors){
    if(!index||index_sectors<n||config(s,b,id,n,r,ctx))return -1;
    return cow_store_recover_indexed(s,index,index_sectors);
}
int cow_store_format(struct cow_store *s,const struct cow_bank b[2],uint64_t id,uint32_t n,int (*r)(void *,uint32_t,void *),void *ctx){
    if(config(s,b,id,n,r,ctx))return -1;
    for(int i=0;i<2;i++)if(s->bank[i].reset(s->bank[i].io.ctx)||s->bank[i].io.sync(s->bank[i].io.ctx))return -1;
    if(header(s,0,1)||cow_mount(&s->view,log_io(&s->bank[0]),id,n,r,ctx)||cow_begin(&s->view)||cow_commit(&s->view))return -1;
    return cow_store_recover(s);
}
int cow_store_compact(struct cow_store *s){
    if(!s||s->faulted||s->view.active||s->selected<0||s->epoch==UINT64_MAX)return -1;
    uint32_t *index=s->view.index;
    int target=1-s->selected;s->faulted=1;struct cow next;
    struct cow_bank *b=&s->bank[target];
    if(b->reset(b->io.ctx)||b->io.sync(b->io.ctx)||header(s,target,s->epoch+1))return -1;
    if(cow_mount(&next,log_io(b),s->base_id,s->sectors,s->base_read,s->base_ctx)||cow_begin(&next))return -1;
    unsigned char data[512],base[512];
    for(uint32_t lba=0;lba<s->sectors;lba++){
        if(cow_read(&s->view,lba,data)||s->base_read(s->base_ctx,lba,base))return -1;
        if(memcmp(data,base,512)&&cow_write(&next,lba,data))return -1;
    }
    if(cow_commit(&next))return -1;
    /* A durable first transaction publishes the new epoch. Never erase old here. */
    s->view=next;s->selected=target;s->epoch++;s->faulted=0;
    if(index&&cow_index(&s->view,index,s->sectors)){s->faulted=1;return -1;}
    return 0;
}
int cow_store_reserve(struct cow_store *s,uint32_t records){
    if(!s||s->faulted||s->view.active||records<2||records>s->view.log.capacity/COW_RECORD)return -1;
    s->reserve_records=records;return 0;
}
int cow_store_begin(void *ctx){
    struct cow_store *s=ctx;
    if(!s||s->faulted||s->view.active)return -1;
    if(s->view.sealed||s->view.generation==UINT32_MAX||(s->view.log.capacity-s->view.end)/COW_RECORD<s->reserve_records)
        if(cow_store_compact(s))return -1;
    if((s->view.log.capacity-s->view.end)/COW_RECORD<s->reserve_records)return -1;
    return cow_begin(&s->view);
}
int cow_store_commit(void *ctx){
    struct cow_store *s=ctx;if(!s||s->faulted)return -1;
    if(cow_commit(&s->view)){s->faulted=1;return -1;}
    return 0;
}
int cow_store_abort(void *ctx){
    struct cow_store *s=ctx;
    if(cow_store_recover(s))return -1;
    if(s->view.sealed)return cow_store_compact(s);
    return 0;
}
