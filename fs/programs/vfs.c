#include <m5os/program_fs.h>
#include <m5os/errno.h>
#include <m5os/kmalloc.h>
#include <m5os/mutex.h>
#include <m5os/app.h>
#include <string.h>

typedef struct {
    char name[PROGRAM_STORE_NAME];uint8_t *data;
    size_t size,pos;uint32_t flags;bool dirty;int error;
} handle;
static program_store *store;
static mutex_t lock=MUTEX_INIT;
static unsigned opened;
static bool mounted,writer;
static void *allocate(size_t n) { void *p=kmalloc_psram(n);return p?p:kmalloc(n); }
static const char *name_of(const char *rel)
{
    if(!rel || rel[0]!='/' || !program_store_path_valid(rel+1)) return NULL;
    return rel+1;
}
static int directory(const char *name)
{
    if(!*name) return 1;
    int index=program_store_find(store,name);
    if(index>=0) return store->entry[index].directory;
    return index==ERR_NOENT && (!strcmp(name,"bas") || !strcmp(name,"scm"));
}
static int parent(const char *name)
{
    const char *slash=strrchr(name,'/');if(!slash) return ERR_OK;
    char path[PROGRAM_STORE_NAME];size_t n=(size_t)(slash-name);memcpy(path,name,n);path[n]=0;
    if(directory(path)) return ERR_OK;
    int index=program_store_find(store,path);return index>=0?ERR_NOTDIR:index;
}
static const char *basename_of(const char *name) {const char *p=strrchr(name,'/');return p?p+1:name;}
static int open_file(void *unused,const char *rel,uint32_t flags,void **out)
{
    (void)unused;const char *name=name_of(rel);if(!name) return ERR_INVAL;
    if(!(flags&(VFS_O_READ|VFS_O_WRITE)) || flags&~(VFS_O_READ|VFS_O_WRITE|VFS_O_CREATE|VFS_O_TRUNC|VFS_O_APPEND)) return ERR_INVAL;
    bool writes=flags&VFS_O_WRITE;
    if(!writes && flags&(VFS_O_CREATE|VFS_O_TRUNC|VFS_O_APPEND)) return ERR_INVAL;
    if((flags&VFS_O_READ)&&!task_has_perm(APP_PERM_FS_READ)) return ERR_PERM;
    if(writes&&!task_has_perm(APP_PERM_FS_WRITE)) return ERR_PERM;
    mutex_lock(&lock);
    int rc=mounted&&store->ready ? ERR_OK : ERR_IO;
    if(rc==ERR_OK&&writes&&writer) rc=ERR_BUSY;
    if(rc==ERR_OK&&directory(name)) rc=ERR_ISDIR;
    if(rc==ERR_OK) rc=parent(name);
    int index=rc==ERR_OK ? program_store_find(store,name) : rc;
    if(rc==ERR_OK && index<0 && !(index==ERR_NOENT&&(flags&VFS_O_CREATE))) rc=index;
    handle *f=NULL;
    if(rc==ERR_OK) { f=allocate(sizeof *f);if(!f) rc=ERR_NOMEM; }
    if(rc==ERR_OK) {
        memset(f,0,sizeof *f);memcpy(f->name,name,strlen(name)+1);f->flags=flags;
        f->size=index>=0 ? store->entry[index].size : 0;
        if(writes && !(flags&VFS_O_TRUNC) && f->size>PROGRAM_STORE_MAX) rc=ERR_NOSPC;
        if(rc==ERR_OK) f->data=allocate(writes ? PROGRAM_STORE_MAX : (f->size?f->size:1));
        if(rc==ERR_OK&&!f->data) rc=ERR_NOMEM;
        else if(rc==ERR_OK && index>=0 && !(flags&VFS_O_TRUNC)) rc=program_store_read(store,name,f->data,f->size,&f->size);
        if(flags&VFS_O_TRUNC) f->size=0;
        f->dirty=writes && (index==ERR_NOENT || (flags&VFS_O_TRUNC));
        if(flags&VFS_O_APPEND) f->pos=f->size;
    }
    if(rc==ERR_OK) { if(writes) writer=true;opened++;*out=f; }
    else if(f) { kfree(f->data);kfree(f); }
    mutex_unlock(&lock);return rc;
}
static int close_file(void *file)
{
    handle *f=file;mutex_lock(&lock);int rc=ERR_OK;
    if(f->flags&VFS_O_WRITE) { if(f->error) rc=f->error;else if(f->dirty) rc=program_store_save(store,f->name,f->data,f->size);writer=false; }
    opened--;mutex_unlock(&lock);kfree(f->data);kfree(f);return rc;
}
static int read_file(void *file,void *out,size_t n)
{
    handle *f=file;if(!(f->flags&VFS_O_READ)||!task_has_perm(APP_PERM_FS_READ)) return ERR_PERM;
    mutex_lock(&lock);
    if(f->pos>=f->size) n=0;
    else if(n>f->size-f->pos) n=f->size-f->pos;
    if(n) memcpy(out,f->data+f->pos,n);
    f->pos+=n;
    mutex_unlock(&lock);return (int)n;
}
static int write_file(void *file,const void *data,size_t n)
{
    handle *f=file;
    if(!(f->flags&VFS_O_WRITE)||!task_has_perm(APP_PERM_FS_WRITE)) {f->error=ERR_PERM;return ERR_PERM;}
    if(f->error) return f->error;
    mutex_lock(&lock);
    if(f->flags&VFS_O_APPEND) f->pos=f->size;
    if(n>PROGRAM_STORE_MAX-f->pos) {f->error=ERR_NOSPC;mutex_unlock(&lock);return ERR_NOSPC;}
    if(n) {
        if(f->pos>f->size) memset(f->data+f->size,0,f->pos-f->size);
        memcpy(f->data+f->pos,data,n);f->pos+=n;if(f->pos>f->size) f->size=f->pos;f->dirty=true;
    }
    mutex_unlock(&lock);return (int)n;
}
static int seek_file(void *file,int32_t offset,int whence)
{
    handle *f=file;mutex_lock(&lock);
    int64_t base=whence==FS_SEEK_SET?0:whence==FS_SEEK_CUR?(int64_t)f->pos:whence==FS_SEEK_END?(int64_t)f->size:-1;
    int64_t pos=base+offset;size_t limit=f->flags&VFS_O_WRITE?PROGRAM_STORE_MAX:f->size;
    if(base<0||pos<0||(uint64_t)pos>limit) {if(f->flags&VFS_O_WRITE) f->error=ERR_INVAL;mutex_unlock(&lock);return ERR_INVAL;}
    f->pos=(size_t)pos;mutex_unlock(&lock);return (int)pos;
}
static void info_for(vfs_info_t *out,const char *name)
{
    int i=program_store_find(store,name);
    *out=(vfs_info_t){.type=directory(name)?FS_TYPE_DIR:FS_TYPE_FILE};
    if(i>=0) out->size=store->entry[i].size;
    const char *base=basename_of(name);memcpy(out->name,base,strlen(base)+1);
}
static int stat_file(void *unused,const char *rel,vfs_info_t *out)
{
    (void)unused;if(!task_has_perm(APP_PERM_FS_READ)) return ERR_PERM;
    mutex_lock(&lock);int rc=mounted&&store->ready?ERR_OK:ERR_IO;
    if(rc==ERR_OK && !strcmp(rel,"/")) {*out=(vfs_info_t){.type=FS_TYPE_DIR};}
    else if(rc==ERR_OK) {
        const char *name=name_of(rel);
        if(!name) rc=ERR_INVAL;
        else if((rc=parent(name))==ERR_OK) {
            int i=program_store_find(store,name);
            if(i<0&&!directory(name)) rc=i;else info_for(out,name);
        }
    }
    mutex_unlock(&lock);return rc;
}
static int list_files(void *unused,const char *rel,int (*cb)(const vfs_info_t *,void *),void *ctx)
{
    (void)unused;if(!task_has_perm(APP_PERM_FS_READ)) return ERR_PERM;
    const char *name=!strcmp(rel,"/")?"":name_of(rel);if(!name) return ERR_INVAL;
    vfs_info_t *list=allocate(sizeof(*list)*(PROGRAM_STORE_FILES+2));if(!list) return ERR_NOMEM;
    unsigned count=0;size_t prefix=strlen(name);
    mutex_lock(&lock);int rc=mounted&&store->ready?ERR_OK:ERR_IO;
    if(!rc&&!directory(name)) rc=ERR_NOTDIR;
    if(!rc) {
        if(!prefix) {info_for(&list[count++],"bas");info_for(&list[count++],"scm");}
        for(unsigned i=0;i<PROGRAM_STORE_FILES;i++) if(store->entry[i].valid&&!store->entry[i].deleted) {
            const char *key=store->entry[i].name,*child=key;
            if(prefix) {
                if(strncmp(key,name,prefix)||key[prefix]!='/') continue;
                child=key+prefix+1;
            } else if(!strcmp(key,"bas")||!strcmp(key,"scm")) continue;
            if(!*child||strchr(child,'/')) continue;
            info_for(&list[count++],key);
        }
        opened++;
    }
    mutex_unlock(&lock);if(rc<0) {kfree(list);return rc;}
    for(unsigned i=0;i<count;i++) if(cb&&cb(&list[i],ctx)) break;
    mutex_lock(&lock);opened--;mutex_unlock(&lock);kfree(list);return (int)count;
}
static int mkdir_file(void *unused,const char *rel)
{
    (void)unused;if(!task_has_perm(APP_PERM_FS_WRITE)) return ERR_PERM;
    const char *name=name_of(rel);if(!name) return ERR_INVAL;
    mutex_lock(&lock);int rc=!mounted||!store->ready?ERR_IO:writer?ERR_BUSY:ERR_OK;
    if(!rc) {
        int index=program_store_find(store,name);
        if(index>=0||directory(name)) rc=ERR_EXIST;
        else if((rc=parent(name))==ERR_OK) rc=program_store_mkdir(store,name);
    }
    mutex_unlock(&lock);return rc;
}
static int unlink_file(void *unused,const char *rel)
{
    (void)unused;if(!task_has_perm(APP_PERM_FS_WRITE)) return ERR_PERM;
    const char *name=name_of(rel);if(!name) return ERR_INVAL;
    mutex_lock(&lock);int rc=!mounted||!store->ready?ERR_IO:writer?ERR_BUSY:ERR_OK;
    if(!rc) {
        if(directory(name)) {
            if(!strcmp(name,"bas")||!strcmp(name,"scm")) rc=ERR_PERM;
            size_t n=strlen(name);
            for(unsigned i=0;!rc&&i<PROGRAM_STORE_FILES;i++) if(store->entry[i].valid&&!store->entry[i].deleted&&
                !strncmp(store->entry[i].name,name,n)&&store->entry[i].name[n]=='/') rc=ERR_NOTEMPTY;
        }
        if(!rc) rc=program_store_delete(store,name);
    }
    mutex_unlock(&lock);return rc;
}
static const vfs_fs_t fs={.name="data",.open=open_file,.close=close_file,.read=read_file,.write=write_file,
    .seek=seek_file,.stat=stat_file,.readdir=list_files,.mkdir=mkdir_file,.unlink=unlink_file};
int program_fs_mount(const program_store_io *io)
{
    if(!task_has_perm(APP_PERM_FS_READ)) return ERR_PERM;
    mutex_lock(&lock);
    if(mounted) {mutex_unlock(&lock);return ERR_BUSY;}
    program_store *candidate=allocate(sizeof *candidate);
    int rc=candidate?program_store_mount(candidate,io):ERR_NOMEM;
    if(!rc) {
        store=candidate;
        for(unsigned i=0;!rc&&i<PROGRAM_STORE_FILES;i++) if(store->entry[i].valid&&!store->entry[i].deleted) rc=parent(store->entry[i].name);
        if(!rc) rc=vfs_mount("/data",&fs,NULL);
    }
    if(!rc) mounted=true;else {store=NULL;kfree(candidate);}
    mutex_unlock(&lock);return rc;
}
int program_fs_init(const program_store_io *io)
{
    if(!task_has_perm(APP_PERM_FS_WRITE)) return ERR_PERM;
    mutex_lock(&lock);
    if(mounted) {mutex_unlock(&lock);return ERR_BUSY;}
    program_store *candidate=allocate(sizeof *candidate);
    int rc=candidate?program_store_init(candidate,io):ERR_NOMEM;
    kfree(candidate);mutex_unlock(&lock);return rc;
}
int program_fs_unmount(void)
{
    if(!task_has_perm(APP_PERM_FS_WRITE)) return ERR_PERM;
    mutex_lock(&lock);int rc=!mounted?ERR_NOENT:opened?ERR_BUSY:vfs_umount("/data");
    if(!rc) {mounted=false;kfree(store);store=NULL;}
    mutex_unlock(&lock);return rc;
}
bool program_fs_mounted(void) { return mounted; }
bool program_fs_ready(void) {mutex_lock(&lock);bool ready=mounted&&store&&store->ready;mutex_unlock(&lock);return ready;}
unsigned program_fs_recovered(void) {mutex_lock(&lock);unsigned n=store?store->recovered:0;mutex_unlock(&lock);return n;}
