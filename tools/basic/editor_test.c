#include "../../user/edit/edit.h"
#include "../../user/tui/dqwin.h"
#include <m5os/vfs.h>
#include <m5os/errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

static union { max_align_t alignment; char bytes[30000]; } cache;
static char disk[EDIT_MAX+1]; static size_t disk_len, offset;
static const int *keys; static unsigned keypos, keycount, opens, closes, reads, messages;
static int deny_open, fail_write, fail_close, live;
static size_t chunk=2;
void *kmalloc_psram(size_t n) { assert(n<=sizeof cache.bytes); return cache.bytes; }
int ksnprintf(char *buf,size_t cap,const char *fmt,...) {
    va_list args; va_start(args,fmt); int n=vsnprintf(buf,cap,fmt,args); va_end(args);
    return n<(int)cap ? n : (int)cap-1;
}
int dq_cols(void) { return 40; } int dq_rows(void) { return 18; }
void dq_text(int x,int y,const char *s) { (void)x;(void)y;(void)s; }
void dq_text_rev(int x,int y,const char *s) { (void)x;(void)y;(void)s; }
void dq_window(int x,int y,int w,int h) { (void)x;(void)y;(void)w;(void)h; }
bool dq_confirm(const char *s) { (void)s; return true; }
void dq_message(const char *a,const char *b) { (void)a;(void)b; messages++; }
int dq_getkey(void) { assert(keypos<keycount); return keys[keypos++]; }
int vfs_stat(const char *p,vfs_info_t *info) { (void)p; *info=(vfs_info_t){.type=FS_TYPE_FILE,.size=(uint32_t)disk_len}; return 0; }
int vfs_open(const char *p,uint32_t flags) {
    (void)p; assert(!live); opens++;
    if ((int)opens==deny_open) return ERR_PERM;
    live=1; offset=0; if (flags&VFS_O_TRUNC) disk_len=0; return 3;
}
int vfs_read(int fd,void *buf,size_t n) {
    assert(fd==3&&live); reads++;
    if (n>disk_len-offset) n=disk_len-offset;
    memcpy(buf,disk+offset,n); offset+=n; return (int)n;
}
int vfs_write(int fd,const void *buf,size_t n) {
    assert(fd==3&&live);
    if ((int)opens==fail_write && offset>=2) return 0;
    if (n>chunk) n=chunk;
    assert(offset+n<=sizeof disk); memcpy(disk+offset,buf,n); offset+=n; disk_len=offset; return (int)n;
}
int vfs_close(int fd) { assert(fd==3&&live); live=0; closes++; return (int)opens==fail_close ? ERR_IO : 0; }
static void setup(const int *sequence,unsigned count) {
    assert(!live); keys=sequence; keycount=count; keypos=opens=closes=reads=messages=0;
    deny_open=fail_write=fail_close=0; disk_len=3; memcpy(disk,"old",3);
}
#define RUN_KEYS(...) const int sequence[]={__VA_ARGS__}; setup(sequence,sizeof sequence/sizeof *sequence)
int main(void) {
    char source[]="10 PRINT 42\n"; size_t len=strlen(source);
    { RUN_KEYS(17); assert(!edit_file_with_text("/home/a.bas",source,len)); assert(!opens && !reads); }
    { RUN_KEYS(19,17); assert(edit_file_with_text("/home/a.bas",source,len)); assert(disk_len==len && !memcmp(disk,source,len)); assert(opens==1 && closes==1 && !reads); }
    { RUN_KEYS('X',17); assert(!edit_file_with_text("/home/a.bas",source,len)); assert(!opens); }
    { RUN_KEYS('X',19,17); assert(edit_file_with_text("/home/a.bas",source,len)); assert(disk_len==len+1 && disk[0]=='X' && !memcmp(disk+1,source,len)); }
    { RUN_KEYS(19,17); deny_open=1; assert(!edit_file_with_text("/sd/a.bas",source,len)); assert(disk_len==3 && !memcmp(disk,"old",3)); }
    { RUN_KEYS(19,17); fail_write=1; assert(!edit_file_with_text("/home/a.bas",source,len)); assert(closes==1 && disk_len==2); }
    { RUN_KEYS(19,17); fail_close=1; assert(!edit_file_with_text("/home/a.bas",source,len)); assert(closes==1); }
    /* A later failed save must not trigger BASIC reload after an earlier success. */
    { RUN_KEYS(19,'X',19,17); fail_write=2; assert(!edit_file_with_text("/home/a.bas",source,len)); assert(closes==2); }
    { RUN_KEYS(19,'X',19,19,17); fail_write=2; assert(edit_file_with_text("/home/a.bas",source,len)); assert(closes==3); }
    { RUN_KEYS(19,17); assert(!edit_file_with_text("/home/a.bas","\0",1)); assert(messages==1 && !opens); }
    { RUN_KEYS(19,17); assert(!edit_file_with_text("/home/a.bas",source,EDIT_MAX+1)); assert(messages==1 && !opens); }
    /* Existing edit_file still loads from disk and saves normally. */
    { RUN_KEYS(19,17); assert(edit_file("/home/a.bas")); assert(reads && disk_len==3 && !memcmp(disk,"old",3)); }
    assert(!strcmp(source,"10 PRINT 42\n"));
    puts("BASIC editor handoff tests PASS");
}

void ime_clear_anchor(void) {}
void ime_set_anchor(int x,int y) { (void)x; (void)y; }
