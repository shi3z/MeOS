#pragma once
#include <m5os/types.h>
/* Portable, caller-serialized store. Relative offsets only; no device writes
 * happen on mount. Provisioning requires explicit init of an entirely blank
 * region. Every file has two 20 KiB banks, commit marker written last. */
#define PROGRAM_STORE_BYTES (1024u*1024u)
#define PROGRAM_STORE_SECTOR 4096u
#define PROGRAM_STORE_BANK (5u*PROGRAM_STORE_SECTOR)
#define PROGRAM_STORE_FILES 16u
#define PROGRAM_STORE_HEADER 64u
#define PROGRAM_STORE_MAX (PROGRAM_STORE_BANK-PROGRAM_STORE_HEADER)
#define PROGRAM_STORE_NAME 24u

/* I/O callbacks return 0 on complete success or a negative ERR_* code. */
typedef struct {
    void *ctx;
    int (*read)(void *,uint32_t offset,void *,size_t length);
    int (*write)(void *,uint32_t offset,const void *,size_t length);
    int (*erase)(void *,uint32_t sector_offset);
} program_store_io;
typedef struct {
    char name[PROGRAM_STORE_NAME];
    uint32_t generation,size,checksum;
    uint8_t bank,valid,deleted;
} program_store_entry;
typedef struct {
    program_store_io io;
    program_store_entry entry[PROGRAM_STORE_FILES];
    bool ready;
    unsigned recovered;
} program_store;
int program_store_init(program_store *,const program_store_io *); /* blank only */
int program_store_mount(program_store *,const program_store_io *);
int program_store_find(const program_store *,const char *name);
int program_store_read(program_store *,const char *name,void *data,size_t capacity,size_t *size);
int program_store_save(program_store *,const char *name,const void *data,size_t size);
int program_store_delete(program_store *,const char *name);

/* Tab5 opt-in partition policy: data subtype 0x42, label programs, exactly
 * 0xC00000..0xCFFFFF, flags zero, no overlaps, backed by sufficient flash. */
int program_store_partition(const uint8_t table[4096],uint32_t chip_size,uint32_t *base);
