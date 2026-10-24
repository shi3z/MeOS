#ifndef MEOS_COW_STORE_H
#define MEOS_COW_STORE_H
#include "cow.h"
/* Two independent, preallocated banks. reset must zero the entire target bank;
 * it must never touch the other bank. IO/sync contract is the same as cow_io.
 * format is EXPLICITLY DESTRUCTIVE. open/recover never format. Caller serializes.
 * Bank epochs identify completed compactions; transactions use cow generations. */
struct cow_bank {
    struct cow_io io;
    int (*reset)(void *);
};
struct cow_store {
    struct cow_bank bank[2];
    struct cow view;
    uint64_t base_id, epoch;
    uint32_t sectors, reserve_records;
    int selected, faulted;
    int (*base_read)(void *,uint32_t,void *);
    void *base_ctx;
};
int cow_store_open(struct cow_store *, const struct cow_bank[2], uint64_t,
                   uint32_t, int (*)(void *,uint32_t,void *),void *);
int cow_store_open_indexed(struct cow_store *, const struct cow_bank[2], uint64_t,
                   uint32_t, int (*)(void *,uint32_t,void *),void *,uint32_t *,size_t);
int cow_store_format(struct cow_store *, const struct cow_bank[2], uint64_t,
                     uint32_t, int (*)(void *,uint32_t,void *),void *);
int cow_store_recover(struct cow_store *);
int cow_store_recover_indexed(struct cow_store *,uint32_t *,size_t);
int cow_store_compact(struct cow_store *);
/* Minimum free log records (including commit) before starting a transaction.
 * This is a capacity budget, not an automatic callback retry. */
int cow_store_reserve(struct cow_store *, uint32_t records);
int cow_store_begin(void *);
int cow_store_commit(void *);
/* Abandon pending writes, reselect durable state, compact a dirty tail, and
 * permit further writes. An uncertain commit may recover old OR new state. */
int cow_store_abort(void *);
#endif
