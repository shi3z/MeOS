#include "../../user/basic/file.h"
#include <m5os/vfs.h>
#include <m5os/errno.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char disk[BASIC_PROGRAM_CAP+16];
static size_t disk_size, offset;
static unsigned opens, closes, allocations;
static int live_fd, readonly, stat_rc, open_rc, read_fail, write_fail, close_fail, oom;
static size_t chunk, reported_size;
static fs_type_t kind;
void *kmalloc_psram(size_t n) { if (oom) return NULL; allocations++; return malloc(n); }
void *kmalloc(size_t n) { if (oom) return NULL; allocations++; return malloc(n); }
void kfree(void *p) { if (p) { assert(allocations); allocations--; free(p); } }
int vfs_stat(const char *path, vfs_info_t *info) {
    assert(path[0]=='/'); if (stat_rc) return stat_rc;
    *info=(vfs_info_t){.type=kind,.size=(uint32_t)reported_size}; return ERR_OK;
}
int vfs_open(const char *path, uint32_t flags) {
    (void)path; opens++; assert(!live_fd);
    if (open_rc) return open_rc;
    if ((flags&VFS_O_WRITE) && readonly) return ERR_PERM;
    live_fd=1; offset=0;
    if (flags&VFS_O_TRUNC) disk_size=0;
    return 9;
}
int vfs_read(int fd, void *buf, size_t len) {
    assert(fd==9 && live_fd);
    if (read_fail && offset>=3) return ERR_IO;
    if (len>chunk) len=chunk;
    if (len>disk_size-offset) len=disk_size-offset;
    memcpy(buf,disk+offset,len); offset+=len; return (int)len;
}
int vfs_write(int fd, const void *buf, size_t len) {
    assert(fd==9 && live_fd);
    if (write_fail && offset>=3) return write_fail==1 ? ERR_IO : 0;
    if (len>chunk) len=chunk;
    assert(offset+len<=sizeof disk); memcpy(disk+offset,buf,len);
    offset+=len; if (offset>disk_size) disk_size=offset; return (int)len;
}
int vfs_close(int fd) {
    assert(fd==9 && live_fd); live_fd=0; closes++;
    return close_fail ? ERR_IO : ERR_OK;
}
static void output(const char *p,size_t n,void *opaque) { (void)p; (void)n; (void)opaque; }
static void setup(const char *data,size_t length) {
    assert(!live_fd && !allocations); assert(length<=sizeof disk);
    memcpy(disk,data,length); reported_size=disk_size=length;
    offset=opens=closes=0; chunk=3; kind=FS_TYPE_FILE;
    readonly=stat_rc=open_rc=read_fail=write_fail=close_fail=oom=0;
}
static void keep(basic_session *s,const basic_program *before,int rc) {
    assert(rc<0); assert(!memcmp(&s->source,before,sizeof *before));
    assert(ubasic_get_variable(0)==9); assert(!allocations && !live_fd);
    basic_session_submit(s,"Z=B(0)"); assert(ubasic_get_variable(25)==55.25);
}
int main(void) {
    basic_session *s=malloc(sizeof *s); basic_program *before=malloc(sizeof *before); assert(s&&before);
    basic_session_init(s,output,NULL,NULL);
    basic_session_submit(s,"10 PRINT 99"); basic_session_submit(s,"A=9");
    memcpy(before,&s->source,sizeof *before);
    basic_file_result r;
    const char crlf[]="\xef\xbb\xbf" "30 END\r\n\r\n10 FOR I=1 TO 2\r\n20 NEXT I";
    setup(crlf,sizeof crlf-1);
    assert(!basic_file_load(s,"/sd/DEMO.BAS",&r));
    assert(s->source.count==3 && s->source.lines[0].number==10 && !strcmp(s->source.lines[2].text,"END"));
    assert(ubasic_get_variable(0)==0 && opens==1 && closes==1 && !allocations);
    assert(!basic_session_run(s));
    size_t len=basic_session_export(s);
    assert(!strcmp(s->program,"10 FOR I=1 TO 2\n20 NEXT I\n30 END\n"));
    setup("old",3);
    assert(!basic_file_save(s,"/home/demo.bas",&r));
    assert(disk_size==len && !memcmp(disk,s->program,len)); assert(r.bytes==len && closes==1);
    /* Re-establish source + variables to verify all failures preserve both. */
    basic_session_submit(s,"A=9"); basic_session_submit(s,"DIM B(0)");
    basic_session_submit(s,"B(0)=55.25"); memcpy(before,&s->source,sizeof *before);
    const char *bad[]={"PRINT 7\n","10 PRINT 1\n10 END\n","0 END\n","65536 END\n","10\n","10 PRINT 1\nRUN\n","10 PRINT 1\nBYE\n"};
    for (unsigned i=0;i<sizeof bad/sizeof *bad;i++) {
        setup(bad[i],strlen(bad[i])); keep(s,before,basic_file_load(s,"/home/bad.bas",&r)); assert(r.line && r.detail);
    }
    const char nul[]="10 PRINT 1\n\0" "20 END\n";
    setup(nul,sizeof nul-1); keep(s,before,basic_file_load(s,"/bad",&r)); assert(r.line==2);
    char large[BASIC_PROGRAM_CAP+1]; memset(large,' ',sizeof large);
    setup(large,sizeof large); keep(s,before,basic_file_load(s,"/large",&r)); assert(!opens);
    setup(large,sizeof large); reported_size=1; keep(s,before,basic_file_load(s,"/grown",&r)); assert(closes==1);
    char overline[140]; memset(overline,' ',sizeof overline); memcpy(overline,"10 REM ",7);
    setup(overline,sizeof overline); keep(s,before,basic_file_load(s,"/long",&r));
    setup("10 END\n",7); read_fail=1; keep(s,before,basic_file_load(s,"/badread",&r)); assert(closes==1);
    setup("10 END\n",7); close_fail=1; keep(s,before,basic_file_load(s,"/badclose",&r));
    setup("10 END\n",7); oom=1; keep(s,before,basic_file_load(s,"/oom",&r)); assert(!opens);
    setup("10 END\n",7); open_rc=ERR_IO; keep(s,before,basic_file_load(s,"/open",&r)); assert(!closes);
    setup("10 END\n",7); stat_rc=ERR_NOENT; keep(s,before,basic_file_load(s,"/missing",&r)); assert(!opens);
    setup("",0); kind=FS_TYPE_DEV; keep(s,before,basic_file_load(s,"/dev/uart",&r)); assert(!opens);
    keep(s,before,basic_file_save(s,"/dev/uart",&r)); assert(!opens);
    setup("original",8); readonly=1; keep(s,before,basic_file_save(s,"/sd/DEMO.BAS",&r)); assert(disk_size==8 && !memcmp(disk,"original",8));
    setup("original",8); write_fail=1; keep(s,before,basic_file_save(s,"/partial",&r)); assert(r.bytes==3 && closes==1);
    setup("original",8); write_fail=2; keep(s,before,basic_file_save(s,"/full",&r)); assert(r.bytes==3 && closes==1);
    setup("original",8); close_fail=1; keep(s,before,basic_file_save(s,"/close",&r)); assert(closes==1);
    setup("",0); stat_rc=ERR_NOENT; assert(!basic_file_save(s,"/new",&r)); assert(r.bytes==len);
    /* A deliberate empty-file LOAD clears the program and variables. */
    setup("",0); assert(!basic_file_load(s,"/empty",&r)); assert(!s->source.count && !ubasic_get_variable(0));
    basic_session_submit(s,"Z=B(0)"); assert(ubasic_error() && strstr(ubasic_error(),"not DIMensioned"));
    assert(!basic_session_run(s));
    const char *examples[]={"user/basic/examples/squares.bas","user/basic/examples/factorial.bas"};
    for (unsigned i=0;i<2;i++) {
        FILE *fp=fopen(examples[i],"rb"); assert(fp);
        char data[1024]; size_t n=fread(data,1,sizeof data,fp); assert(!ferror(fp) && feof(fp)); fclose(fp);
        setup(data,n); assert(!basic_file_load(s,"/home/example.bas",&r)); assert(!basic_session_run(s));
        assert(i ? ubasic_get_variable(0)==720 : ubasic_get_variable(8)==6);
    }
    const char *decimal_source="10 DIM A(1)\n20 A(0)=1.25e-3\n30 A(1)=A(0)/2\n40 PRINT A(1)\n";
    setup(decimal_source,strlen(decimal_source));
    assert(!basic_file_load(s,"/home/decimal.bas",&r)); assert(!basic_session_run(s));
    basic_session_submit(s,"Z=A(1)");
    double decimal_value=ubasic_get_variable(25); assert(decimal_value>.000624999999999 && decimal_value<.000625000000001);
    assert(!basic_file_save(s,"/home/decimal.bas",&r));
    assert(disk_size==strlen(decimal_source) && !memcmp(disk,decimal_source,disk_size));
    free(before); free(s); puts("BASIC file tests PASS");
}

int vfs_replace_file(const char *path,const void *data,size_t len,size_t *written)
{
    return vfs_write_file(path,data,len,written);
}
