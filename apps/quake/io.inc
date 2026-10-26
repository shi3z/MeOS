/* Private read-only stdio handles. Never pass them to newlib. */
typedef struct { int fd; bool live,error; uint32_t size; } quake_file;
static quake_file files[12];
static quake_file *stream(FILE *f) {
    for(unsigned i=0;i<12;i++) if(f==(FILE *)&files[i] && files[i].live) return &files[i];
    return NULL;
}
static FILE *open_readonly(const char *path,const char *mode) {
    char normalized[VFS_PATH_MAX];
    if(strcmp(mode,"r") && strcmp(mode,"rb")) return NULL;
    size_t path_len=strlen(path);
    if(path_len>=4 && !strcmp(path+path_len-4,".sav")) return NULL; /* save parser is not enabled */
    if(vfs_normalize(path,normalized)<0 || (strncmp(normalized,"/sd/",4)&&strncmp(normalized,"/assets/quake/",14))) return NULL;
    vfs_info_t info;
    if(vfs_stat(normalized,&info)<0 || info.type!=FS_TYPE_FILE || info.size>INT32_MAX) return NULL;
    for(unsigned i=0;i<12;i++) if(!files[i].live) {
        int fd=vfs_open(normalized,VFS_O_READ); if(fd<0) return NULL;
        files[i]=(quake_file){.fd=fd,.live=true,.size=info.size}; return (FILE *)&files[i];
    }
    return NULL;
}
FILE *quake_fopen(const char *path,const char *mode) {
    QUAKE_IO_BEGIN;FILE *f=open_readonly(path,mode);QUAKE_IO_END(QIO_OPEN,0);return f;
}
int quake_fclose(FILE *file) { quake_file *f=stream(file);if(!f)return EOF;int rc=vfs_close(f->fd);f->live=false;return rc<0?EOF:0; }
void quake_io_close_all(void) { for(unsigned i=0;i<12;i++) if(files[i].live) quake_fclose((FILE *)&files[i]); }
size_t quake_fread(void *buf,size_t size,size_t count,FILE *file) {
    quake_file *f=stream(file); if(!f || !size || count>SIZE_MAX/size) return 0;
    size_t n=size*count,done=0;
    while(done<n) {
        size_t chunk=n-done;if(chunk>4096)chunk=4096;
        QUAKE_IO_BEGIN;
        int got=vfs_read(f->fd,(char *)buf+done,chunk);
        QUAKE_IO_END(QIO_READ,got>0?(unsigned)got:0);
        if(got<0) { f->error=true;break; } if(!got)break;
        done+=(unsigned)got;
    }
    return done/size;
}
size_t quake_fwrite(const void *p,size_t s,size_t n,FILE *file) { (void)p;(void)s;(void)n;quake_file *f=stream(file);if(f)f->error=true;return 0; }
static int seek_file(quake_file *f,int32_t off,int whence){
    QUAKE_IO_BEGIN;int rc=vfs_seek(f->fd,off,whence);QUAKE_IO_END(QIO_SEEK,0);return rc;
}
int quake_fseek(FILE *file,long off,int whence) { quake_file *f=stream(file);if(!f||off<INT32_MIN||off>INT32_MAX)return -1;int rc=seek_file(f,(int32_t)off,whence);if(rc<0)f->error=true;return rc<0?-1:0; }
long quake_ftell(FILE *file) { quake_file *f=stream(file);return f?seek_file(f,0,SEEK_CUR):-1; }
int quake_feof(FILE *file) { quake_file *f=stream(file);long pos=quake_ftell(file);return !f||pos<0||(unsigned long)pos>=f->size; }
int quake_ferror(FILE *file) { quake_file *f=stream(file);return !f||f->error; }
int quake_fgetc(FILE *file) { uint8_t b;return quake_fread(&b,1,1,file)==1?b:EOF; }
int quake_fputc(int c,FILE *file) { (void)c;(void)file;return EOF; }
int quake_fflush(FILE *file) { (void)file;return 0; }
double quake_atof(const char *s) { while(*s==' '||*s=='\t')s++;const char *end;double v=0;return number_parse(s,&end,&v)?0:v; }
