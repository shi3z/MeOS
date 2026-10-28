#pragma once
#include <m5os/types.h>

/*
 * Kernel memory allocation.
 *
 * Two independent heaps, because the ESP32-S3 has two very different RAMs:
 *
 *   internal SRAM  - fast, always accessible (also from ISRs / with the flash
 *                    cache off), usable by DMA.  Small (we reserve a static
 *                    arena).  Task stacks MUST live here.
 *   PSRAM (8 MB)   - external, behind the data cache, ~10x slower, large.
 *                    Good for framebuffers, file buffers, big tables.
 *
 * All blocks are 16-byte aligned.  All functions are safe to call from tasks
 * and interrupt context (interrupts are masked inside).
 */

void  kmem_init(void);

void *kmalloc(size_t size);             /* internal first; PSRAM if internal is exhausted */
void *kcalloc(size_t n, size_t size);   /* zeroed, same policy as kmalloc */
void *kmalloc_internal(size_t size);    /* internal SRAM only (NULL when full) */
void *kmalloc_internal_owned(size_t size, uint32_t owner);   /* internal, tagged for app `owner` */
void *kmalloc_psram_owned(size_t size, uint32_t owner);       /* PSRAM, tagged for app `owner` (NULL without PSRAM) */
void *umalloc_psram(size_t size);                              /* PSRAM only, tagged with the calling app */
void *kmalloc_psram(size_t size);       /* PSRAM only (NULL if absent or full) */
void *kmalloc_dma(size_t size);         /* DMA-capable: internal, 16-byte aligned */
void *umalloc(size_t size);             /* for applications: tagged with the app, reclaimed when it ends */
void  kfree(void *p);                   /* also frees umalloc() blocks (ufree is an alias) */
#define ufree  kfree
size_t kmem_reclaim_owner(uint32_t app_id);   /* kernel: free everything an app still holds */
size_t kmem_owned_bytes(uint32_t app_id);                   /* works for blocks from any kmalloc_*; NULL ok */

typedef struct {
    size_t total;          /* bytes managed                                   */
    size_t used;           /* bytes in allocated blocks (incl. headers)       */
    size_t free;           /* bytes in free blocks                            */
    size_t largest_free;   /* biggest single allocation that would succeed    */
    size_t peak_used;
    uint32_t alloc_count;  /* successful allocations so far                   */
    uint32_t fail_count;   /* failed allocations so far                       */
} kmem_stats_t;

void kmem_internal_stats(kmem_stats_t *out);
void kmem_psram_stats(kmem_stats_t *out);    /* total == 0 when PSRAM is absent */
bool kmem_selftest(void);                    /* prints results; true on pass    */
