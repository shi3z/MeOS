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
    if(!rel || rel[0]!='/' || !rel[1] || strchr(rel+1,'/')) return NULL;
    return rel+1;
}
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
    int index=rc==ERR_OK ? program_store_find(store,name) : rc;
    if(rc==ERR_OK && index<0 && !(index==ERR_NOENT&&(flags&VFS_O_CREATE))) rc=index;
    handle *f=NULL;
    if(rc==ERR_OK) { f=allocate(sizeof *f);if(!f) rc=ERR_NOMEM; }
    if(rc==ERR_OK) {
        memset(f,0,sizeof *f);memcpy(f->name,name,strlen(name)+1);f->flags=flags;
        f->size=index>=0 ? store->entry[index].size : 0;
        f->data=allocate(writes ? PROGRAM_STORE_MAX : (f->size?f->size:1));
        if(!f->data) rc=ERR_NOMEM;
        else if(index>=0 && !(flags&VFS_O_TRUNC)) rc=program_store_read(store,name,f->data,f->size,&f->size);
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
static int stat_file(void *unused,const char *rel,vfs_info_t *out)
{
    (void)unused;if(!task_has_perm(APP_PERM_FS_READ)) return ERR_PERM;
    mutex_lock(&lock);int rc=mounted&&store->ready?ERR_OK:ERR_IO;
    if(rc==ERR_OK && !strcmp(rel,"/")) {memset(out,0,sizeof *out);out->type=FS_TYPE_DIR;}
    else if(rc==ERR_OK) {
        const char *name=name_of(rel);int i=name?program_store_find(store,name):ERR_INVAL;
        if(i<0) rc=i;else {*out=(vfs_info_t){.type=FS_TYPE_FILE,.size=store->entry[i].size};memcpy(out->name,name,strlen(name)+1);}
    }
    mutex_unlock(&lock);return rc;
}
static int list_files(void *unused,const char *rel,int (*cb)(const vfs_info_t *,void *),void *ctx)
{
    (void)unused;if(!task_has_perm(APP_PERM_FS_READ)) return ERR_PERM;
    if(strcmp(rel,"/")) return ERR_NOTDIR;
    /* VFS entries now hold UTF-8 long names; keep snapshots off small task stacks. */
    vfs_info_t *list=kmalloc_psram(sizeof(*list)*PROGRAM_STORE_FILES);
    if(!list) return ERR_NOMEM;
    unsigned count=0;
    mutex_lock(&lock);int rc=mounted&&store->ready?ERR_OK:ERR_IO;
    if(!rc) for(unsigned i=0;i<PROGRAM_STORE_FILES;i++) if(store->entry[i].valid&&!store->entry[i].deleted) {
        list[count]=(vfs_info_t){.type=FS_TYPE_FILE,.size=store->entry[i].size};
        memcpy(list[count++].name,store->entry[i].name,PROGRAM_STORE_NAME);
    }
    /* Pin the mount but call outside its lock, matching VFS readdir users. */
    if(!rc) opened++;
    mutex_unlock(&lock);if(rc<0) {kfree(list);return rc;}
    for(unsigned i=0;i<count;i++) if(cb&&cb(&list[i],ctx)) break;
    mutex_lock(&lock);opened--;mutex_unlock(&lock);kfree(list);return (int)count;
}
static int unlink_file(void *unused,const char *rel)
{
    (void)unused;if(!task_has_perm(APP_PERM_FS_WRITE)) return ERR_PERM;
    const char *name=name_of(rel);if(!name) return ERR_INVAL;
    mutex_lock(&lock);int rc=mounted&&!writer?program_store_delete(store,name):ERR_BUSY;
    mutex_unlock(&lock);return rc;
}
static const vfs_fs_t fs={.name="programs",.open=open_file,.close=close_file,.read=read_file,.write=write_file,
    .seek=seek_file,.stat=stat_file,.readdir=list_files,.unlink=unlink_file};
int program_fs_mount(const program_store_io *io)
{
    if(!task_has_perm(APP_PERM_FS_READ)) return ERR_PERM;
    mutex_lock(&lock);
    if(mounted) {mutex_unlock(&lock);return ERR_BUSY;}
    program_store *candidate=allocate(sizeof *candidate);
    int rc=candidate?program_store_mount(candidate,io):ERR_NOMEM;
    if(!rc) rc=vfs_mount("/programs",&fs,NULL);
    if(!rc) {store=candidate;mounted=true;} else kfree(candidate);
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
    mutex_lock(&lock);int rc=!mounted?ERR_NOENT:opened?ERR_BUSY:vfs_umount("/programs");
    if(!rc) {mounted=false;kfree(store);store=NULL;}
    mutex_unlock(&lock);return rc;
}
bool program_fs_mounted(void) { return mounted; }
bool program_fs_ready(void) {mutex_lock(&lock);bool ready=mounted&&store&&store->ready;mutex_unlock(&lock);return ready;}
unsigned program_fs_recovered(void) {mutex_lock(&lock);unsigned n=store?store->recovered:0;mutex_unlock(&lock);return n;}
