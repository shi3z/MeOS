#include "../../fs/cow/cow.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define CAP (16*COW_RECORD)
static unsigned char disk[CAP], stable[CAP], baseline[CAP];
static int budget, fail_sync, sync_count, buffered, indexed;
static int rd(void *ctx,uint32_t off,void *p,size_t n) {
    (void)ctx;assert(off<=CAP&&n<=CAP-off);memcpy(p,disk+off,n);return 0;
}
static int wr(void *ctx,uint32_t off,const void *p,size_t n) {
    (void)ctx;assert(off<=CAP&&n<=CAP-off);
    size_t take=n;if(budget>=0&&(size_t)budget<take)take=(size_t)budget;
    memcpy(disk+off,p,take);
    /* Worst case for torn writes: every accepted byte is immediately durable. */
    if(!buffered)memcpy(stable+off,p,take);
    if(budget>=0)budget-=(int)take;
    return take==n?0:-1;
}
static int sync_io(void *ctx) {
    (void)ctx;sync_count++;if(sync_count==fail_sync)return -1;
    memcpy(stable,disk,CAP);return 0;
}
static int base(void *ctx,uint32_t lba,void *out) {
    (void)ctx;memset(out,(int)lba,512);return 0;
}
static struct cow_io io={0,CAP,rd,wr,sync_io};
static void mount(struct cow *c) {assert(!cow_mount(c,io,42,8,base,0));}
static int tx(struct cow *c,int v) {
    unsigned char b[512];memset(b,v,512);
    if(cow_begin(c)||cow_write(c,1,b)||cow_write(c,2,b))return -1;
    return cow_commit(c);
}
static void check(struct cow *c,int v) {
    unsigned char b[512];for(unsigned lba=1;lba<=2;lba++) {
        assert(!cow_read(c,lba,b));for(int i=0;i<512;i++)assert(b[i]==v);
    }
}
int main(void) {
    struct cow c;budget=-1;mount(&c);assert(!tx(&c,11));check(&c,11);
    memcpy(baseline,stable,CAP);
    uint32_t offsets[8];
    for(indexed=0;indexed<=1;indexed++)
    for(buffered=0;buffered<=1;buffered++)
    for(int cut=0;cut<=3*(int)COW_RECORD;cut++) {
        memcpy(disk,baseline,CAP);memcpy(stable,baseline,CAP);
        budget=cut;fail_sync=0;sync_count=0;mount(&c);if(indexed)assert(!cow_index(&c,offsets,8));int rc=tx(&c,22);
        memcpy(disk,stable,CAP);mount(&c);
        assert(c.generation==1||c.generation==2);
        check(&c,c.generation==2?22:11);
        if(!rc)assert(c.generation==2);
        unsigned char b[512];assert(!cow_read(&c,3,b));assert(b[0]==3);
    }
    buffered=0;
    for(int phase=1;phase<=2;phase++) {
        memcpy(disk,baseline,CAP);memcpy(stable,baseline,CAP);
        budget=-1;sync_count=0;fail_sync=phase;mount(&c);
        assert(tx(&c,22));assert(c.sealed);assert(cow_begin(&c));
        memcpy(disk,stable,CAP);mount(&c);check(&c,c.generation==2?22:11);
    }
    fail_sync=0;budget=-1;memcpy(disk,baseline,CAP);
    assert(cow_mount(&c,io,43,8,base,0));
    mount(&c);assert(cow_write(&c,1,baseline));assert(!cow_begin(&c));
    assert(cow_write(&c,8,baseline));
    assert(!cow_write(&c,1,baseline)); /* uncommitted complete data */
    mount(&c);assert(c.sealed);check(&c,11);assert(cow_begin(&c));
    memcpy(disk,baseline,CAP);disk[COW_RECORD+100]^=1;
    mount(&c);assert(c.sealed&&c.generation==0);
    memset(disk,0,CAP);mount(&c);budget=-1;
    while(!tx(&c,33)){}
    assert(c.sealed);mount(&c);assert(c.generation>0);check(&c,33);
    puts("COW PASS: 6628 byte-cut scenarios (indexed/unindexed, immediate/buffered durability), sync failures, old/new atomic recovery, base identity, corruption, bounds, incomplete tail, capacity; ASan/UBSan");
}
