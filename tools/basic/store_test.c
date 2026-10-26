#include <m5os/program_store.h>
#include <m5os/errno.h>
#include <m5os/crc32.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t flash[PROGRAM_STORE_BYTES],baseline[PROGRAM_STORE_BYTES];
static unsigned mutations,fail_at;static int tear,fail_reads;
static int read_io(void *ctx,uint32_t off,void *out,size_t n) {
    (void)ctx;assert(off<=sizeof flash && n<=sizeof flash-off);
    if(fail_reads) return ERR_IO;
    if(n) memcpy(out,flash+off,n);
    return 0;
}
static int write_io(void *ctx,uint32_t off,const void *data,size_t n) {
    (void)ctx;assert(!(off&3)&&!(n&3)&&off<=sizeof flash&&n<=sizeof flash-off);
    int fail=++mutations==fail_at;
    size_t count=fail?(tear==2?n:tear==1?n/2:0):n;
    for(size_t i=0;i<count;i++) {assert((flash[off+i]&((const uint8_t *)data)[i])==((const uint8_t *)data)[i]);flash[off+i]&=((const uint8_t *)data)[i];}
    return fail?ERR_IO:0;
}
static int erase_io(void *ctx,uint32_t off) {
    (void)ctx;assert(!(off&4095)&&off<=sizeof flash-4096);int fail=++mutations==fail_at;
    memset(flash+off,255,fail?(tear==2?4096:tear==1?2048:0):4096);return fail?ERR_IO:0;
}
static program_store_io io={.read=read_io,.write=write_io,.erase=erase_io};
static void reset_fault(void) {mutations=fail_at=0;tear=fail_reads=0;}
static void put32(uint8_t *p,uint32_t n) {for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(n>>(i*8));}
static void record_crc(uint8_t *h) {put32(h+112,crc32_update(crc32(h+4,108),h+116,12));}
static void expect(program_store *s,const char *name,const char *data) {
    char got[100];size_t len;assert(!program_store_read(s,name,got,sizeof got,&len));assert(len==strlen(data)&&!memcmp(got,data,len));
}
static void partition_entry(uint8_t *p,const char *name,unsigned type,unsigned sub,uint32_t off,uint32_t size) {
    memset(p,0,32);p[0]=0xaa;p[1]=0x50;p[2]=type;p[3]=sub;put32(p+4,off);put32(p+8,size);memcpy(p+12,name,strlen(name));
}
int main(void) {
    program_store s,reboot;memset(flash,255,sizeof flash);reset_fault();
    assert(program_store_mount(&s,&io)==ERR_NOENT && !mutations);
    assert(!program_store_init(&s,&io)&&s.ready);assert(program_store_init(&s,&io)==ERR_EXIST);
    assert(!program_store_mount(&s,&io));
    assert(!program_store_save(&s,"a.bas","10 PRINT 1\n",11));expect(&s,"a.bas","10 PRINT 1\n");
    assert(!program_store_save(&s,"b.scm","(+ 1 2)",7));
    assert(!program_store_save(&s,"a.bas","10 PRINT 2\n",11));
    assert(!program_store_mount(&reboot,&io));expect(&reboot,"a.bas","10 PRINT 2\n");expect(&reboot,"b.scm","(+ 1 2)");
    assert(!program_store_save(&reboot,"empty.bas",NULL,0));size_t n=999;assert(!program_store_read(&reboot,"empty.bas",NULL,0,&n)&&!n);
    assert(program_store_save(&reboot,"../bad",NULL,0)==ERR_INVAL);
    assert(program_store_save(&reboot,"long-long-long-long-name.bas",NULL,0)==ERR_INVAL);
    assert(!program_store_delete(&reboot,"empty.bas"));assert(program_store_find(&reboot,"empty.bas")==ERR_NOENT);
    memcpy(baseline,flash,sizeof flash);
    reset_fault();assert(!program_store_save(&reboot,"a.bas","new version",11));unsigned total=mutations;
    /* Every erase/write boundary, including partial effects and a reported
     * failure after a complete physical mutation. A reboot sees old or new. */
    for(unsigned cut=1;cut<=total;cut++) for(int effect=0;effect<3;effect++) {
        memcpy(flash,baseline,sizeof flash);reset_fault();assert(!program_store_mount(&s,&io));
        fail_at=cut;tear=effect;assert(program_store_save(&s,"a.bas","new version",11)<0);
        assert(!s.ready && program_store_save(&s,"b.scm","x",1)==ERR_IO);
        reset_fault();assert(!program_store_mount(&reboot,&io));
        char data[40];size_t len;assert(!program_store_read(&reboot,"a.bas",data,sizeof data,&len));
        assert(len==11 && (!memcmp(data,"new version",11)||!memcmp(data,"10 PRINT 2\n",11)));
        expect(&reboot,"b.scm","(+ 1 2)");
    }
    uint8_t *replacement=malloc(PROGRAM_STORE_MAX);assert(replacement);memset(replacement,'P',PROGRAM_STORE_MAX);
    memcpy(flash,baseline,sizeof flash);reset_fault();assert(!program_store_mount(&s,&io));
    assert(!program_store_save(&s,"a.bas",replacement,PROGRAM_STORE_MAX));unsigned large_cuts=mutations;
    uint8_t *restored=malloc(PROGRAM_STORE_MAX);assert(restored);
    for(unsigned cut=1;cut<=large_cuts;cut++) for(int effect=0;effect<3;effect++) {
        memcpy(flash,baseline,sizeof flash);reset_fault();assert(!program_store_mount(&s,&io));fail_at=cut;tear=effect;
        assert(program_store_save(&s,"a.bas",replacement,PROGRAM_STORE_MAX)<0);
        reset_fault();assert(!program_store_mount(&reboot,&io));size_t length;
        assert(!program_store_read(&reboot,"a.bas",restored,PROGRAM_STORE_MAX,&length));
        assert((length==11&&!memcmp(restored,"10 PRINT 2\n",11)) || (length==PROGRAM_STORE_MAX&&!memcmp(restored,replacement,length)));
        expect(&reboot,"b.scm","(+ 1 2)");
    }
    free(replacement);free(restored);
    /* First creation and deletion are atomic under the same failures. */
    for(int deleting=0;deleting<2;deleting++) {
        memcpy(flash,baseline,sizeof flash);reset_fault();assert(!program_store_mount(&s,&io));
        assert(!(deleting?program_store_delete(&s,"a.bas"):program_store_save(&s,"fresh.bas","hello",5)));unsigned count=mutations;
        for(unsigned cut=1;cut<=count;cut++) for(int effect=0;effect<3;effect++) {
            memcpy(flash,baseline,sizeof flash);reset_fault();assert(!program_store_mount(&s,&io));fail_at=cut;tear=effect;
            assert((deleting?program_store_delete(&s,"a.bas"):program_store_save(&s,"fresh.bas","hello",5))<0);
            reset_fault();assert(!program_store_mount(&reboot,&io));
            int id=program_store_find(&reboot,deleting?"a.bas":"fresh.bas");assert(id>=0||id==ERR_NOENT);
            if(id>=0) expect(&reboot,deleting?"a.bas":"fresh.bas",deleting?"10 PRINT 2\n":"hello");
            expect(&reboot,"b.scm","(+ 1 2)");
        }
    }
    memcpy(flash,baseline,sizeof flash);reset_fault();assert(!program_store_mount(&s,&io));
    /* Corrupt the newest payload: recover its older validated bank. */
    int id=program_store_find(&s,"a.bas");uint32_t newest=4096+((unsigned)id*2+s.entry[id].bank)*PROGRAM_STORE_BANK;
    flash[newest+PROGRAM_STORE_HEADER]^=1;
    char corrupted[32];size_t ignored;assert(program_store_read(&s,"a.bas",corrupted,sizeof corrupted,&ignored)==ERR_CORRUPT&&!s.ready);
    assert(program_store_save(&s,"a.bas","x",1)==ERR_IO);
    assert(!program_store_mount(&s,&io)&&s.recovered==1);expect(&s,"a.bas","10 PRINT 1\n");
    flash[4096+((unsigned)id*2+(s.entry[id].bank))*PROGRAM_STORE_BANK+PROGRAM_STORE_HEADER]^=1;
    assert(program_store_mount(&s,&io)==ERR_CORRUPT&&!s.ready);
    /* Capacity, slot reuse and stale generations. */
    memset(flash,255,sizeof flash);reset_fault();assert(!program_store_init(&s,&io));
    for(unsigned i=0;i<PROGRAM_STORE_FILES;i++) {char name[24];snprintf(name,sizeof name,"p%u.bas",i);assert(!program_store_save(&s,name,"x",1));}
    assert(program_store_save(&s,"full.bas","x",1)==ERR_NOSPC);
    assert(!program_store_delete(&s,"p0.bas"));assert(!program_store_save(&s,"reused.bas","ok",2));
    assert(!program_store_mount(&s,&io));expect(&s,"reused.bas","ok");
    uint8_t *large=malloc(PROGRAM_STORE_MAX+1);assert(large);memset(large,'a',PROGRAM_STORE_MAX+1);
    assert(program_store_save(&s,"reused.bas",large,PROGRAM_STORE_MAX+1)==ERR_NOSPC);
    assert(!program_store_save(&s,"reused.bas",large,PROGRAM_STORE_MAX));
    memset(large,0,PROGRAM_STORE_MAX);assert(!program_store_read(&s,"reused.bas",large,PROGRAM_STORE_MAX,&n)&&n==PROGRAM_STORE_MAX);
    assert(large[0]=='a'&&large[n-1]=='a');free(large);
    id=program_store_find(&s,"reused.bas");uint8_t *h=flash+4096+((unsigned)id*2+s.entry[id].bank)*PROGRAM_STORE_BANK;
    put32(h+8,UINT32_MAX);record_crc(h);assert(!program_store_mount(&s,&io));assert(program_store_save(&s,"reused.bas","x",1)==ERR_NOSPC);
    reset_fault();fail_reads=1;assert(program_store_mount(&s,&io)==ERR_IO);reset_fault();
    memset(flash,255,sizeof flash);flash[800000]=1;assert(program_store_init(&s,&io)==ERR_EXIST&&!mutations);
    /* Init never erases; interrupted provisioning is not auto-formatted. */
    memset(flash,255,sizeof flash);reset_fault();fail_at=1;tear=1;assert(program_store_init(&s,&io)==ERR_IO);
    reset_fault();assert(program_store_mount(&s,&io)==ERR_CORRUPT);assert(program_store_init(&s,&io)==ERR_EXIST&&!mutations);
    /* Read legacy 64-byte records, then update atomically to path-aware v2. */
    memset(flash,255,sizeof flash);reset_fault();assert(!program_store_init(&s,&io));
    uint8_t legacy[64];memset(legacy,255,sizeof legacy);
    put32(legacy,0x4752504d);put32(legacy+4,1);put32(legacy+8,1);put32(legacy+12,3);
    put32(legacy+16,crc32("old",3));put32(legacy+20,0);memset(legacy+24,0,24);memcpy(legacy+24,"legacy.bas",10);
    put32(legacy+48,crc32_update(crc32(legacy+4,44),legacy+52,12));memcpy(flash+4096,legacy,64);memcpy(flash+4096+64,"old",3);
    assert(!program_store_mount(&s,&io));expect(&s,"legacy.bas","old");assert(s.entry[0].header_size==64);
    memcpy(baseline,flash,sizeof flash);reset_fault();assert(!program_store_save(&s,"legacy.bas","new",3));unsigned legacy_cuts=mutations;
    for(unsigned cut=1;cut<=legacy_cuts;cut++) for(int effect=0;effect<3;effect++) {
        memcpy(flash,baseline,sizeof flash);reset_fault();assert(!program_store_mount(&s,&io));fail_at=cut;tear=effect;
        assert(program_store_save(&s,"legacy.bas","new",3)<0);reset_fault();assert(!program_store_mount(&reboot,&io));
        char text[4];size_t length;assert(!program_store_read(&reboot,"legacy.bas",text,sizeof text,&length));
        assert(length==3&&(!memcmp(text,"old",3)||!memcmp(text,"new",3)));
    }
    reset_fault();assert(!program_store_mkdir(&reboot,"lua"));assert(!program_store_save(&reboot,"lua/hello.lua","print(1)",8));
    assert(!program_store_mount(&s,&io));assert(s.entry[program_store_find(&s,"lua")].directory);
    expect(&s,"lua/hello.lua","print(1)");assert(program_store_read(&s,"lua",legacy,sizeof legacy,&n)==ERR_ISDIR);
    const char *bad_paths[]={"/absolute","bas//bad","bas/../bad","bas/./bad","trailing/"};
    for(unsigned i=0;i<sizeof bad_paths/sizeof *bad_paths;i++) assert(program_store_save(&s,bad_paths[i],"x",1)==ERR_INVAL);
    uint8_t table[4096];uint32_t base=7;memset(table,255,sizeof table);
    partition_entry(table,"factory",0,0,0x10000,0x3e0000);partition_entry(table+32,"wasmapps",1,0x41,0xa00000,0x200000);
    assert(program_store_partition(table,0x1000000,&base)==ERR_NOENT&&base==7);
    partition_entry(table+64,"programs",1,0x42,0xc00000,PROGRAM_STORE_BYTES);
    assert(!program_store_partition(table,0x1000000,&base)&&base==0xc00000);
    assert(program_store_partition(table,0xc00000,&base)==ERR_NODEV);
    put32(table+64+28,1);assert(program_store_partition(table,0x1000000,&base)==ERR_INVAL);put32(table+64+28,0);
    partition_entry(table+96,"overlap",1,0x43,0xbff000,8192);assert(program_store_partition(table,0x1000000,&base)==ERR_CORRUPT);
    printf("program store transaction/power-cut/partition tests PASS (%u small / %u max-size cut points)\n",total,large_cuts);
}
