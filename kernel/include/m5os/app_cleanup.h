#pragma once
#include <m5os/types.h>
#define APP_CLEANUP_MAX 8u
typedef int (*app_cleanup_fn)(uint32_t owner);
/* Register once during kernel startup, before sealing. Callbacks must be
 * idempotent and stop DMA/workers before returning success. No callbacks in IRQs. */
int app_cleanup_register(const char *name, app_cleanup_fn callback);
void app_cleanup_seal(void);
/* Runs every hook even after a failure; returns first error. Failure means
 * owner memory MUST remain quarantined and cleanup may be retried. */
int app_cleanup_run(uint32_t owner);
void app_cleanup_register_builtins(void);
