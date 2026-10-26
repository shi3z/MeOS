#include <m5os/program_store.h>
#include <m5os/crc32.h>
#include <m5os/errno.h>
#include <string.h>

#define FORMAT_MAGIC 0x4652504du /* MPRF, little endian */
#define FILE_MAGIC 0x4752504du   /* MPRG */
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static void put32(uint8_t *p,uint32_t n) { for(unsigned i=0;i<4;i++) p[i]=(uint8_t)(n>>(8*i)); }
static uint32_t header_crc(const uint8_t *h) { return crc32_update(crc32(h+4,44),h+52,12); }
static uint32_t bank_offset(unsigned slot,unsigned bank) { return PROGRAM_STORE_SECTOR+(slot*2u+bank)*PROGRAM_STORE_BANK; }
bool program_store_path_valid(const char *name)
{
    size_t length=0,part=0;const char *component=name;
    for(;;) {
        unsigned char c=name[length];
        if(!c || c=='/') {
            if(!part || part>PROGRAM_STORE_COMPONENT ||
               (part==1&&component[0]=='.') || (part==2&&component[0]=='.'&&component[1]=='.')) return 0;
            if(!c) return 1;
            component=name+length+1;part=0;
        } else {
            if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-'||c=='.')) return 0;
            part++;
        }
        if(++length>=PROGRAM_STORE_NAME) return 0;
    }
}
static uint32_t record_crc(const uint8_t *h) {return crc32_update(crc32(h+4,108),h+116,12);}
static void format_header(uint8_t h[64])
{
    memset(h,255,64);put32(h,FORMAT_MAGIC);put32(h+4,1);
    put32(h+8,PROGRAM_STORE_SECTOR);put32(h+12,PROGRAM_STORE_BANK);
    put32(h+16,PROGRAM_STORE_FILES);put32(h+20,PROGRAM_STORE_BYTES);
    put32(h+48,header_crc(h));
}
static int io_ok(const program_store_io *io) { return io&&io->read&&io->write&&io->erase; }
int program_store_init(program_store *s,const program_store_io *io)
{
    if(!io_ok(io)) return ERR_INVAL;
    program_store_io copied=*io;memset(s,0,sizeof *s);s->io=copied;io=&s->io;
    uint8_t bytes[256];
    for(uint32_t off=0;off<PROGRAM_STORE_BYTES;off+=sizeof bytes) {
        if(io->read(io->ctx,off,bytes,sizeof bytes)<0) return ERR_IO;
        for(unsigned i=0;i<sizeof bytes;i++) if(bytes[i]!=255) return ERR_EXIST;
    }
    uint8_t header[64];format_header(header);
    if(io->write(io->ctx,4,header+4,60)<0 || io->write(io->ctx,0,header,4)<0) return ERR_IO;
    return program_store_mount(s,io);
}
static int data_crc(program_store *s,uint32_t off,uint32_t length,uint32_t *result)
{
    uint8_t bytes[256];uint32_t c=0;
    for(uint32_t pos=0;pos<length;) {
        size_t n=length-pos;if(n>sizeof bytes) n=sizeof bytes;
        if(s->io.read(s->io.ctx,off+pos,bytes,n)<0) return ERR_IO;
        c=crc32_update(c,bytes,n);pos+=(uint32_t)n;
    }
    *result=c;return ERR_OK;
}
/* 0 no committed record, 1 valid record, negative: I/O or committed corruption. */
static int read_record(program_store *s,unsigned slot,unsigned bank,program_store_entry *e)
{
    uint8_t h[PROGRAM_STORE_HEADER];uint32_t off=bank_offset(slot,bank);
    if(s->io.read(s->io.ctx,off,h,64)<0) return ERR_IO;
    if(get32(h)!=FILE_MAGIC) return 0;
    uint32_t version=get32(h+4);unsigned header,name_size;
    if(version==1) {
        header=64;name_size=24;
        if(get32(h+48)!=header_crc(h)) return ERR_CORRUPT;
    } else if(version==2) {
        header=PROGRAM_STORE_HEADER;name_size=PROGRAM_STORE_NAME;
        if(s->io.read(s->io.ctx,off+64,h+64,header-64)<0) return ERR_IO;
        if(get32(h+112)!=record_crc(h)) return ERR_CORRUPT;
    } else return ERR_CORRUPT;
    if(!memchr(h+24,0,name_size)) return ERR_CORRUPT;
    uint32_t length=get32(h+12),flags=get32(h+20),c;
    uint32_t limit=version==1?PROGRAM_STORE_LEGACY_MAX:PROGRAM_STORE_MAX;
    if(!program_store_path_valid((const char *)h+24) || !get32(h+8) || length>limit || flags>(version==1?1u:3u) || (flags&&length)) return ERR_CORRUPT;
    int rc=data_crc(s,off+header,length,&c);if(rc<0) return rc;
    if(c!=get32(h+16)) return ERR_CORRUPT;
    memset(e,0,sizeof *e);memcpy(e->name,h+24,name_size);
    e->generation=get32(h+8);e->size=length;e->checksum=c;e->deleted=flags&1;e->directory=(flags>>1)&1;
    e->bank=(uint8_t)bank;e->valid=1;e->header_size=(uint16_t)header;
    return 1;
}
int program_store_mount(program_store *s,const program_store_io *io)
{
    if(!io_ok(io)) return ERR_INVAL;
    /* io may alias s->io on remount. */
    program_store_io copied=*io;memset(s,0,sizeof *s);s->io=copied;
    uint8_t h[64],expected[64];format_header(expected);
    if(s->io.read(s->io.ctx,0,h,sizeof h)<0) return ERR_IO;
    if(memcmp(h,expected,sizeof h)) {
        bool blank=true;for(unsigned i=0;i<sizeof h;i++) if(h[i]!=255) blank=false;
        return blank ? ERR_NOENT : ERR_CORRUPT;
    }
    for(unsigned i=0;i<PROGRAM_STORE_FILES;i++) {
        program_store_entry a,b;
        int ra=read_record(s,i,0,&a),rb=read_record(s,i,1,&b);
        if(ra==ERR_IO || rb==ERR_IO) return ERR_IO;
        if(ra!=1 && rb!=1) { if(ra<0 || rb<0) return ERR_CORRUPT;continue; }
        if(ra<0 || rb<0) s->recovered++;
        if(ra==1 && rb==1 && a.generation==b.generation &&
           (strcmp(a.name,b.name)||a.size!=b.size||a.checksum!=b.checksum||a.deleted!=b.deleted||a.directory!=b.directory)) return ERR_CORRUPT;
        s->entry[i]=rb==1 && (ra!=1 || b.generation>a.generation) ? b : a;
        if(!s->entry[i].deleted) for(unsigned j=0;j<i;j++)
            if(s->entry[j].valid&&!s->entry[j].deleted&&!strcmp(s->entry[j].name,s->entry[i].name)) return ERR_CORRUPT;
    }
    s->ready=true;return ERR_OK;
}
int program_store_find(const program_store *s,const char *name)
{
    if(!s->ready) return ERR_IO;
    if(!program_store_path_valid(name)) return ERR_INVAL;
    for(unsigned i=0;i<PROGRAM_STORE_FILES;i++) if(s->entry[i].valid&&!s->entry[i].deleted&&!strcmp(s->entry[i].name,name)) return (int)i;
    return ERR_NOENT;
}
int program_store_read(program_store *s,const char *name,void *data,size_t capacity,size_t *size)
{
    int i=program_store_find(s,name);if(i<0) return i;
    program_store_entry *e=&s->entry[i];if(e->directory) return ERR_ISDIR;
    if(capacity<e->size || (!data&&e->size)) return ERR_NOSPC;
    if(e->size && s->io.read(s->io.ctx,bank_offset((unsigned)i,e->bank)+e->header_size,data,e->size)<0) {s->ready=false;return ERR_IO;}
    if(crc32(data,e->size)!=e->checksum) {s->ready=false;return ERR_CORRUPT;}
    *size=e->size;return ERR_OK;
}
static int commit(program_store *s,const char *name,const void *data,size_t size,bool deleted,bool directory)
{
    int index=program_store_find(s,name);
    if(index<0 && index!=ERR_NOENT) return index;
    if(index>=0&&!deleted) {
        if(directory) return ERR_EXIST;
        if(s->entry[index].directory) return ERR_ISDIR;
    }
    if(size>PROGRAM_STORE_MAX || (!data&&size)) return ERR_NOSPC;
    if(index==ERR_NOENT) {
        if(deleted) return ERR_NOENT;
        for(unsigned i=0;i<PROGRAM_STORE_FILES;i++) if(!s->entry[i].valid||s->entry[i].deleted) {index=(int)i;break;}
        if(index==ERR_NOENT) return ERR_NOSPC;
    }
    program_store_entry *old=&s->entry[index];
    if(deleted) directory=old->directory;
    if(old->valid && old->generation==UINT32_MAX) return ERR_NOSPC;
    if(old->valid) {
        program_store_entry current;int rc=read_record(s,(unsigned)index,old->bank,&current);
        if(rc!=1 || strcmp(current.name,old->name) || current.generation!=old->generation ||
           current.size!=old->size || current.checksum!=old->checksum || current.deleted!=old->deleted || current.directory!=old->directory) {
            s->ready=false;return rc<0?rc:ERR_CORRUPT;
        }
    }
    unsigned bank=old->valid ? old->bank^1u : 0;
    uint32_t off=bank_offset((unsigned)index,bank),generation=old->valid ? old->generation+1 : 1;
    uint8_t h[PROGRAM_STORE_HEADER];memset(h,255,sizeof h);put32(h,FILE_MAGIC);put32(h+4,2);put32(h+8,generation);
    put32(h+12,(uint32_t)size);put32(h+16,crc32(data,size));put32(h+20,(deleted?1u:0u)|(directory?2u:0u));
    memset(h+24,0,PROGRAM_STORE_NAME);memcpy(h+24,name,strlen(name));put32(h+112,record_crc(h));
    /* Freeze on any uncertain transaction: caller must remount before another
     * write, including failures after the commit word may have reached flash. */
    s->ready=false;
    for(uint32_t p=0;p<PROGRAM_STORE_BANK;p+=PROGRAM_STORE_SECTOR) if(s->io.erase(s->io.ctx,off+p)<0) return ERR_IO;
    if(s->io.write(s->io.ctx,off+4,h+4,sizeof h-4)<0) return ERR_IO;
    for(size_t p=0;p<size;) {
        uint8_t bytes[256];size_t n=size-p;if(n>sizeof bytes) n=sizeof bytes;
        memset(bytes,255,sizeof bytes);memcpy(bytes,(const uint8_t *)data+p,n);
        if(s->io.write(s->io.ctx,off+PROGRAM_STORE_HEADER+(uint32_t)p,bytes,(n+3)&~(size_t)3)<0) return ERR_IO;
        p+=n;
    }
    uint32_t read_crc;if(data_crc(s,off+PROGRAM_STORE_HEADER,(uint32_t)size,&read_crc)<0) return ERR_IO;
    uint8_t verify[PROGRAM_STORE_HEADER];if(s->io.read(s->io.ctx,off,verify,sizeof verify)<0) return ERR_IO;
    if(memcmp(verify+4,h+4,sizeof h-4)||read_crc!=get32(h+16)) return ERR_CORRUPT;
    if(s->io.write(s->io.ctx,off,h,4)<0) return ERR_IO;
    program_store_entry fresh;int rc=read_record(s,(unsigned)index,bank,&fresh);
    if(rc!=1) return rc<0 ? rc : ERR_CORRUPT;
    *old=fresh;s->ready=true;return ERR_OK;
}
int program_store_save(program_store *s,const char *name,const void *data,size_t size) { return commit(s,name,data,size,false,false); }
int program_store_mkdir(program_store *s,const char *name) {return commit(s,name,NULL,0,false,true);}
int program_store_delete(program_store *s,const char *name) { return commit(s,name,NULL,0,true,false); }

int program_store_partition(const uint8_t table[4096],uint32_t chip_size,uint32_t *base)
{
    const uint32_t expected=0x00c00000u;
    if(chip_size<expected+PROGRAM_STORE_BYTES) return ERR_NODEV;
    unsigned found=0;bool terminated=false;
    static const char label[16]="programs";
    for(unsigned i=0;i<4096/32;i++) {
        const uint8_t *e=table+i*32;unsigned magic=e[0]|(unsigned)e[1]<<8;
        if(magic==0xffff || magic==0xebeb) {terminated=true;break;}
        if(magic!=0x50aa) return ERR_CORRUPT;
        uint32_t offset=get32(e+4),size=get32(e+8);
        if(!size || offset>chip_size || size>chip_size-offset) return ERR_CORRUPT;
        if(!memcmp(e+12,label,16)) {
            if(e[2]!=1 || e[3]!=0x42 || offset!=expected || size!=PROGRAM_STORE_BYTES || get32(e+28)) return ERR_INVAL;
            found++;
        } else if(offset<expected+PROGRAM_STORE_BYTES && size>expected-(offset<expected?offset:expected)) {
            /* Overlap expressed without unchecked offset+size arithmetic. */
            return ERR_CORRUPT;
        }
    }
    if(!terminated || found>1) return ERR_CORRUPT;
    if(!found) return ERR_NOENT;
    *base=expected;return ERR_OK;
}
