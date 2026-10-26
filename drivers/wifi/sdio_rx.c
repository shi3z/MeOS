#include "sdio_rx.h"
#include <string.h>

bool wifi_sdio_rx_plan(const wifi_sdio_rx_t *rx, uint32_t pending,
                       size_t *logical, size_t *physical, uint32_t *address)
{
    if (!rx || !logical || !physical || !address || !pending || pending > WIFI_SDIO_FIFO_END ||
        rx->used >= WIFI_SDIO_MAX_FRAME) return false;
    size_t room = (sizeof rx->data - rx->used) / WIFI_SDIO_BLOCK * WIFI_SDIO_BLOCK;
    if (room > WIFI_SDIO_MAX_FRAME) room = WIFI_SDIO_MAX_FRAME;
    *logical = pending < room ? pending : room;
    *physical = (*logical + WIFI_SDIO_BLOCK - 1) / WIFI_SDIO_BLOCK * WIFI_SDIO_BLOCK;
    *address = WIFI_SDIO_FIFO_END - pending;
    return true;
}

bool wifi_sdio_rx_commit(wifi_sdio_rx_t *rx, size_t logical,
                         wifi_sdio_frame_fn frame, void *ctx)
{
    if (!rx || !frame || rx->used >= WIFI_SDIO_MAX_FRAME ||
        logical > sizeof rx->data - rx->used) return false;
    size_t valid = rx->used + logical, pos = 0;
    while (pos + WIFI_SDIO_HEADER <= valid) {
        const uint8_t *h = rx->data + pos;
        size_t len = h[2] | (size_t)h[3] << 8;
        size_t offset = h[4] | (size_t)h[5] << 8;
        if (offset < WIFI_SDIO_HEADER || offset > WIFI_SDIO_MAX_FRAME ||
            !len || len > 4096 || len > WIFI_SDIO_MAX_FRAME - offset) {
            rx->bad_headers++;
            rx->used = 0; /* no trustworthy next-frame boundary in this batch */
            return false;
        }
        if (offset + len > valid - pos) break;
        frame(ctx, h[0] & 15, h + offset, len);
        rx->frames++;
        pos += offset + len;
    }
    rx->used = valid - pos;
    if (rx->used) {
        if (logical) rx->partials++;
        if (pos) memmove(rx->data, rx->data + pos, rx->used);
    }
    return true;
}
