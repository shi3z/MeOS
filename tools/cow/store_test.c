#include "../../fs/cow/store.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define SECTORS 4u
#define CAP (512u+12u*COW_RECORD)
struct bank {unsigned char live[CAP],durable[CAP];};
static struct bank banks[2],saved[2];
static int budget=-1,buffered,fail_sync,syncs,fail_after,fail_read,reads,bank_reads[2];
static int rd(void *ctx,uint32_t o,void *p,size_t n){
    struct bank *b=ctx;assert(o<=CAP&&n<=CAP-o);
    bank_reads[b==&banks[1]]++;
    if(++reads==fail_read)return -1;
    memcpy(p,b->live+o,n);return 0;
}
static int mutate(struct bank *b,uint32_t o,const void *p,size_t n){
    size_t take=n;if(budget>=0&&(size_t)budget<take)take=(size_t)budget;
    if(p)memcpy(b->live+o,p,take);else memset(b->live+o,0,take);
    if(!buffered)memcpy(b->durable+o,b->live+o,take);
    if(budget>=0)budget-=(int)take;
    return take==n?0:-1;
}
static int wr(void *ctx,uint32_t o,const void *p,size_t n){assert(o<=CAP&&n<=CAP-o);return mutate(ctx,o,p,n);}
static int reset(void *ctx){return mutate(ctx,0,0,CAP);}
static int sync_io(void *ctx){
    struct bank *b=ctx;int fail=++syncs==fail_sync;
    if(fail&&!fail_after)return -1;
    memcpy(b->durable,b->live,CAP);return fail?-1:0;
}
static int base(void *ctx,uint32_t lba,void *p){(void)ctx;memset(p,(int)lba,512);return 0;}
static struct cow_bank io[2];
static void open_store(struct cow_store *s){assert(!cow_store_open(s,io,42,SECTORS,base,0));}
static void open_store_indexed(struct cow_store *s,uint32_t index[SECTORS]){assert(!cow_store_open_indexed(s,io,42,SECTORS,base,0,index,SECTORS));}
static void power(void){for(int i=0;i<2;i++)memcpy(banks[i].live,banks[i].durable,CAP);budget=-1;fail_sync=0;syncs=0;}
static void transaction(struct cow_store *s,int value){unsigned char b[512];memset(b,value,512);assert(!cow_store_begin(s));assert(!cow_write(&s->view,1,b));assert(!cow_store_commit(s));}
static void check(struct cow_store *s,int value){unsigned char b[512];
    for(unsigned lba=0;lba<SECTORS;lba++){
        assert(!cow_read(&s->view,lba,b));
        for(int j=0;j<512;j++)assert(b[j]==(lba==1?value:(int)lba));
    }
}
int main(void){
    for(int i=0;i<2;i++)io[i]=(struct cow_bank){{&banks[i],CAP,rd,wr,sync_io},reset};
    struct cow_store s;assert(cow_store_open(&s,io,42,SECTORS,base,0));
    assert(!cow_store_format(&s,io,42,SECTORS,base,0));transaction(&s,11);
    assert(!cow_store_compact(&s));transaction(&s,11);
    memcpy(saved,banks,sizeof saved);
    bank_reads[0]=bank_reads[1]=0;open_store(&s);
    assert(bank_reads[1-s.selected]==1); /* inactive bank header only */
    uint32_t offsets[SECTORS];bank_reads[0]=bank_reads[1]=0;open_store_indexed(&s,offsets);
    assert(bank_reads[1-s.selected]==1);
    assert(bank_reads[s.selected]==2+(int)(s.view.committed/COW_RECORD)); /* header + log + zero tail */
    assert(s.view.index==offsets);check(&s,11);
    /* Both banks already contain published states. Cut every byte of target
     * reuse: reset + header + one changed sector + snapshot commit. */
    unsigned cases=0;unsigned bytes=CAP+512+2*COW_RECORD;
    for(buffered=0;buffered<=1;buffered++)for(unsigned cut=0;cut<=bytes;cut++){
        memcpy(banks,saved,sizeof banks);budget=(int)cut;fail_sync=0;syncs=0;open_store(&s);
        (void)cow_store_compact(&s);power();open_store(&s);check(&s,11);
        assert(!cow_store_abort(&s));transaction(&s,22);check(&s,22);cases++;
    }
    for(buffered=0;buffered<=1;buffered++)for(fail_after=0;fail_after<=1;fail_after++)
    for(int stage=1;stage<=3;stage++){
        memcpy(banks,saved,sizeof banks);open_store(&s);fail_sync=stage;syncs=0;
        assert(cow_store_compact(&s));assert(s.faulted);power();open_store(&s);check(&s,11);
        transaction(&s,22);
    }
    buffered=0;fail_after=0;
    memcpy(banks,saved,sizeof banks);reads=0;fail_read=1;
    assert(cow_store_open(&s,io,42,SECTORS,base,0));assert(s.faulted);
    assert(!memcmp(banks,saved,sizeof banks));fail_read=0;
    memcpy(banks,saved,sizeof banks);open_store(&s);
    unsigned char b[512];memset(b,99,sizeof b);assert(!cow_store_begin(&s));
    assert(!cow_write(&s.view,1,b));assert(!cow_store_abort(&s));check(&s,11);
    transaction(&s,22);assert(!cow_store_compact(&s));transaction(&s,33);
    assert(!cow_store_compact(&s));power();open_store(&s);check(&s,33);
    for(int i=0;i<30;i++){
        if(cow_store_begin(&s)){assert(!cow_store_abort(&s));assert(!cow_store_begin(&s));}
        memset(b,i,512);
        if(cow_write(&s.view,1,b)){assert(!cow_store_abort(&s));transaction(&s,i);}
        else assert(!cow_store_commit(&s));
    }
    power();open_store(&s);check(&s,29);
    transaction(&s,1);assert(!cow_store_compact(&s));power();open_store(&s);check(&s,1);
    /* A too-small inactive bank cannot publish a partial snapshot. */
    memcpy(banks,saved,sizeof banks);open_store(&s);
    memset(b,77,sizeof b);assert(!cow_store_begin(&s));assert(!cow_write(&s.view,2,b));assert(!cow_store_commit(&s));
    int target=1-s.selected;io[target].io.capacity=512+2*COW_RECORD;
    open_store(&s);assert(cow_store_compact(&s));power();open_store(&s);
    assert(!cow_read(&s.view,1,b)&&b[0]==11);assert(!cow_read(&s.view,2,b)&&b[0]==77);
    io[target].io.capacity=CAP;
    assert(!cow_store_abort(&s));transaction(&s,22);
    assert(cow_store_open(&s,io,43,SECTORS,base,0));
    printf("COW STORE PASS: %u compaction byte cuts, sync/read failures, abort/restart, alternating bank reuse, capacity recovery and undersized target\n",cases);
}
