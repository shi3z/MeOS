#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    WIFI_SDIO_RX_BYTES = 8192,
    WIFI_SDIO_BLOCK = 512,
    WIFI_SDIO_MAX_FRAME = WIFI_SDIO_RX_BYTES - WIFI_SDIO_BLOCK,
    WIFI_SDIO_HEADER = 12,
    WIFI_SDIO_FIFO_END = 0x1f800
};

typedef struct {
    uint8_t data[WIFI_SDIO_RX_BYTES];
    size_t used;
    uint32_t frames, partials, bad_headers;
} wifi_sdio_rx_t;

typedef void (*wifi_sdio_frame_fn)(void *ctx, uint8_t interface,
                                  const uint8_t *payload, size_t size);

/* Plan one bounded DMA read. Address encodes the entire remaining slave
 * window, while logical/physical describe this transfer only. */
bool wifi_sdio_rx_plan(const wifi_sdio_rx_t *rx, uint32_t pending,
                       size_t *logical, size_t *physical, uint32_t *address);

/* DMA has written physical bytes at data+used; commit only its logical bytes.
 * The callback borrows the payload and must not recursively feed this stream. */
bool wifi_sdio_rx_commit(wifi_sdio_rx_t *rx, size_t logical,
                         wifi_sdio_frame_fn frame, void *ctx);
