#pragma once
#include <m5os/types.h>
/* Cooperative single-core reservations, not a hardware/security boundary.
 * Reserve the complete controller when its reset/clock affects both RX and TX. */
typedef enum {
    PERIPHERAL_I2S0,
    PERIPHERAL_AHB_DMA_TX2,
    PERIPHERAL_AHB_DMA_RX2,
    PERIPHERAL_COUNT
} peripheral_t;
/* All-or-nothing, nonblocking, idempotent for the same stable driver token.
 * Duplicate/invalid IDs and empty requests are rejected. No reference counts. */
bool peripheral_claim(const peripheral_t *ids, size_t count, const void *owner);
/* Stop hardware/callbacks and free DMA buffers BEFORE releasing reservations.
 * Never release automatically on app death: driver teardown must run first. */
void peripheral_release_owner(const void *owner);
bool peripheral_is_owned(peripheral_t id, const void *owner);
