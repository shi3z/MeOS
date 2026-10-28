#include <m5os/kmalloc.h>
#include <m5os/hal_mem.h>
#include <m5os/kernel.h>
#include <m5os/task.h>
#include <m5os/kprintf.h>
#include "heap.h"

/* Internal heap: a static arena in .bss (internal DRAM).  It never touches
 * ESP-IDF's heap_caps allocator, whose free memory is simply left unused. */
#define KHEAP_INTERNAL_SIZE   (192u * 1024u)

static uint8_t s_internal_arena[KHEAP_INTERNAL_SIZE] __attribute__((aligned(16)));
static heap_t  s_internal;
static heap_t  s_psram;
static bool    s_psram_present;
static uint32_t current_owner(void);

void kmem_init(void)
{
    heap_init(&s_internal, "internal", s_internal_arena, sizeof s_internal_arena);

    uintptr_t base; size_t size;
    hal_psram_region(&base, &size);
    if (size) {
        heap_init(&s_psram, "psram", (void *)base, size);
        s_psram_present = true;
    }
}

void *kmalloc_internal(size_t size) { return heap_alloc(&s_internal, size); }
/* Kernel allocation on behalf of an app (e.g. its loaded code image): tagged like umalloc, so
 * it is reclaimed with the app, but explicit about the owner because the task may not exist yet. */
void *kmalloc_internal_owned(size_t size, uint32_t owner) { return heap_alloc_owned(&s_internal, size, owner); }
void *kmalloc_psram_owned(size_t size, uint32_t owner) { return s_psram_present ? heap_alloc_owned(&s_psram, size, owner) : NULL; }
void *umalloc_psram(size_t size)
{
    return s_psram_present ? heap_alloc_owned(&s_psram, size, current_owner()) : NULL;
}
void *kmalloc_psram(size_t size)    { return s_psram_present ? heap_alloc(&s_psram, size) : NULL; }
/* Internal DRAM is DMA-capable on the S3 and our blocks are 16-byte aligned,
 * which also satisfies the cache-line rules a future PSRAM-DMA path would add. */
void *kmalloc_dma(size_t size)      { return heap_alloc(&s_internal, size); }

void *kmalloc(size_t size)
{
    void *p = heap_alloc(&s_internal, size);
    if (!p && s_psram_present) p = heap_alloc(&s_psram, size);
    return p;
}

void *kcalloc(size_t n, size_t size)
{
    if (size && n > (size_t)-1 / size) return NULL;      /* overflow */
    size_t total = n * size;
    uint8_t *p = kmalloc(total);
    for (size_t i = 0; p && i < total; i++) p[i] = 0;
    return p;
}

void kfree(void *p)
{
    if (!p) return;
    if (heap_contains(&s_internal, p))                   heap_free(&s_internal, p);
    else if (s_psram_present && heap_contains(&s_psram, p)) heap_free(&s_psram, p);
    else kernel_panic("kfree: pointer not from any kernel heap");
}

/* umalloc: like kmalloc, but the block is tagged with the calling app so the kernel can
 * reclaim it when the app ends.  Kernel code keeps using kmalloc (never reclaimed). */
static uint32_t current_owner(void)
{
    task_t *t = task_current();
    return (t && t->app) ? *(uint32_t *)t->app : 0;   /* app_t starts with app_id */
}

void *umalloc(size_t size)
{
    uint32_t owner = current_owner();
    void *p = heap_alloc_owned(&s_internal, size, owner);
    if (!p && s_psram_present) p = heap_alloc_owned(&s_psram, size, owner);
    return p;
}

size_t kmem_reclaim_owner(uint32_t owner)
{
    size_t n = heap_reclaim(&s_internal, owner);
    if (s_psram_present) n += heap_reclaim(&s_psram, owner);
    return n;
}

size_t kmem_owned_bytes(uint32_t owner)
{
    size_t n = heap_owned_bytes(&s_internal, owner);
    if (s_psram_present) n += heap_owned_bytes(&s_psram, owner);
    return n;
}

void kmem_internal_stats(kmem_stats_t *out) { heap_stats(&s_internal, out); }
void kmem_psram_stats(kmem_stats_t *out)
{
    if (s_psram_present) heap_stats(&s_psram, out);
    else *out = (kmem_stats_t){ 0 };
}

/* ---- self test ----------------------------------------------------------- */

#define TEST_SLOTS      48
#define TEST_ROUNDS     3000
#define TEST_MAX_SIZE   1500
#define PSRAM_TEST_SIZE (1024u * 1024u)

static uint32_t s_rng = 0x2545F491u;
static uint32_t rnd(void) { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5; return s_rng; }

static void fill(uint8_t *p, size_t n, uint8_t seed) { for (size_t i = 0; i < n; i++) p[i] = (uint8_t)(seed + i); }
static bool verify(const uint8_t *p, size_t n, uint8_t seed)
{
    for (size_t i = 0; i < n; i++) if (p[i] != (uint8_t)(seed + i)) return false;
    return true;
}

static bool test_heap(heap_t *h, const char *name)
{
    kmem_stats_t before, after;
    heap_stats(h, &before);

    /* 1. coalescing: free in an order that exercises left+right merges */
    void *a = heap_alloc(h, 100), *b = heap_alloc(h, 200), *c = heap_alloc(h, 300);
    if (!a || !b || !c) return false;
    heap_free(h, b); heap_free(h, a); heap_free(h, c);
    heap_stats(h, &after);
    if (after.free != before.free || after.largest_free != before.largest_free) {
        kprintf("  %s: coalescing FAILED (largest %u -> %u)\n", name, (uint32_t)before.largest_free, (uint32_t)after.largest_free);
        return false;
    }

    /* 2. randomized churn with pattern verification */
    static void   *ptr[TEST_SLOTS];
    static size_t  len[TEST_SLOTS];
    static uint8_t seed[TEST_SLOTS];
    for (int i = 0; i < TEST_SLOTS; i++) ptr[i] = NULL;
    for (int r = 0; r < TEST_ROUNDS; r++) {
        int i = (int)(rnd() % TEST_SLOTS);
        if (ptr[i]) {
            if (!verify(ptr[i], len[i], seed[i])) { kprintf("  %s: data corrupted\n", name); return false; }
            heap_free(h, ptr[i]);
            ptr[i] = NULL;
        } else {
            len[i] = 1 + rnd() % TEST_MAX_SIZE;
            seed[i] = (uint8_t)rnd();
            ptr[i] = heap_alloc(h, len[i]);
            if (ptr[i]) fill(ptr[i], len[i], seed[i]);
        }
        if ((r & 0x1FF) == 0 && !heap_check(h)) { kprintf("  %s: heap_check failed at round %d\n", name, r); return false; }
    }
    for (int i = 0; i < TEST_SLOTS; i++) {
        if (ptr[i]) {
            if (!verify(ptr[i], len[i], seed[i])) return false;
            heap_free(h, ptr[i]);
        }
    }

    heap_stats(h, &after);
    if (!heap_check(h) || after.used != before.used || after.free != before.free ||
        after.largest_free != before.largest_free) {
        kprintf("  %s: leak or fragmentation after churn (free %u -> %u, largest %u -> %u)\n", name,
                (uint32_t)before.free, (uint32_t)after.free,
                (uint32_t)before.largest_free, (uint32_t)after.largest_free);
        return false;
    }
    return true;
}

bool kmem_selftest(void)
{
    bool ok = true;
    kmem_stats_t st;

    bool r = test_heap(&s_internal, "internal");
    kprintf("kmem selftest: internal heap  %s\n", r ? "PASS" : "FAIL");
    ok &= r;

    if (s_psram_present) {
        r = test_heap(&s_psram, "psram");
        kprintf("kmem selftest: psram heap     %s\n", r ? "PASS" : "FAIL");
        ok &= r;

        /* one big block: proves the whole mapped range is real RAM */
        uint8_t *big = kmalloc_psram(PSRAM_TEST_SIZE);
        bool big_ok = big != NULL;
        if (big) {
            fill(big, PSRAM_TEST_SIZE, 0x5A);
            big_ok = verify(big, PSRAM_TEST_SIZE, 0x5A);
            kfree(big);
        }
        kprintf("kmem selftest: psram 1 MiB write/verify  %s\n", big_ok ? "PASS" : "FAIL");
        ok &= big_ok;
    } else {
        kprintf("kmem selftest: PSRAM not present, skipped\n");
    }

    void *d = kmalloc_dma(64);
    bool d_ok = d && !((uintptr_t)d & 15u) && heap_contains(&s_internal, d);
    kfree(d);
    kprintf("kmem selftest: dma alloc internal+aligned  %s\n", d_ok ? "PASS" : "FAIL");
    ok &= d_ok;

    kmem_internal_stats(&st);
    kprintf("internal heap: total %u KB  used %u B  peak %u B  largest free %u KB\n",
            (uint32_t)(st.total / 1024), (uint32_t)st.used, (uint32_t)st.peak_used, (uint32_t)(st.largest_free / 1024));
    return ok;
}
