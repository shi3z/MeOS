#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../drivers/wifi/sdio_rx.h"

static uint8_t wire[100000];
static size_t size, received;
static wifi_sdio_rx_t rx;

static void put16(uint8_t *p, unsigned n) { p[0] = (uint8_t)n; p[1] = (uint8_t)(n >> 8); }
static void append(unsigned index, size_t payload, size_t offset)
{
    assert(payload >= 4 && size + offset + payload <= sizeof wire);
    uint8_t *p = wire + size;
    memset(p, 0, offset);
    p[0] = (uint8_t)(index % 6);
    put16(p + 2, (unsigned)payload); put16(p + 4, (unsigned)offset);
    for (size_t j = 0; j < payload; j++) p[offset + j] = (uint8_t)(index * 29 + j);
    p[offset] = (uint8_t)index;
    size += offset + payload;
}
static void frame(void *ctx, uint8_t iface, const uint8_t *p, size_t n)
{
    (void)ctx;
    assert(iface == received % 6 && p[0] == received && n >= 4);
    for (size_t j = 1; j < n; j++) assert(p[j] == (uint8_t)(received * 29 + j));
    received++;
}
static void reset(void) { memset(&rx, 0, sizeof rx); size = received = 0; }
static void transfer(size_t off, size_t n)
{
    assert(n <= sizeof rx.data - rx.used);
    memcpy(rx.data + rx.used, wire + off, n);
    /* DMA padding can resemble a valid frame. It must never be dispatched. */
    size_t padded = (n + 511) / 512 * 512;
    if (padded <= sizeof rx.data - rx.used) memset(rx.data + rx.used + n, 0xa5, padded - n);
    assert(wifi_sdio_rx_commit(&rx, n, frame, NULL));
}
int main(void)
{
    reset();
    for (unsigned i = 0; i < 20; i++) append(i, 1454, 12);
    for (size_t off = 0; off < size;) {
        size_t logical, physical; uint32_t address;
        assert(wifi_sdio_rx_plan(&rx, (uint32_t)(size - off), &logical, &physical, &address));
        assert(address == WIFI_SDIO_FIFO_END - (size - off));
        assert(physical % 512 == 0 && physical <= sizeof rx.data - rx.used);
        transfer(off, logical); off += logical;
    }
    assert(received == 20 && rx.used == 0);
    assert(rx.partials > 0 && !rx.bad_headers);
    /* Every split point, including within a header and at the final byte. */
    for (size_t split = 1; split < 1466; split++) {
        reset(); append(0, 1454, 12); append(1, 1454, 12);
        transfer(0, split); transfer(split, size - split);
        assert(received == 2 && !rx.used && !rx.bad_headers);
    }
    reset(); append(0, 4096, 3584); /* largest supported frame */
    for (size_t off = 0; off < size; off++) transfer(off, 1);
    assert(received == 1 && !rx.used && !rx.bad_headers);
    /* Long mixed stream with reproducible irregular transfer boundaries. */
    for (unsigned trial = 0; trial < 100; trial++) {
        reset();
        for (unsigned i = 0; i < 40; i++) append(i, 4 + (i * 257u + trial) % 1500, 12 + i % 4);
        uint32_t seed = trial + 1;
        for (size_t off = 0; off < size;) {
            size_t n, physical; uint32_t address;
            assert(wifi_sdio_rx_plan(&rx, (uint32_t)(size - off), &n, &physical, &address));
            seed = seed * 1664525u + 1013904223u;
            n = 1 + seed % n;
            transfer(off, n); off += n;
        }
        assert(received == 40 && !rx.used && !rx.bad_headers);
    }
    for (unsigned bad = 0; bad < 4; bad++) {
        reset(); append(0, 20, 12);
        if (bad == 0) put16(wire + 4, 11);
        if (bad == 1) put16(wire + 2, 0);
        if (bad == 2) put16(wire + 2, 4097);
        if (bad == 3) put16(wire + 4, WIFI_SDIO_MAX_FRAME);
        memcpy(rx.data, wire, size);
        assert(!wifi_sdio_rx_commit(&rx, size, frame, NULL));
        assert(rx.bad_headers == 1 && !received && !rx.used);
        size = 0; append(0, 20, 12); transfer(0, size);
        assert(received == 1 && !rx.used); /* a later clean batch remains usable */
    }
    reset();
    size_t logical, physical; uint32_t address;
    assert(!wifi_sdio_rx_plan(&rx, 0, &logical, &physical, &address));
    assert(!wifi_sdio_rx_plan(&rx, WIFI_SDIO_FIFO_END + 1, &logical, &physical, &address));
    assert(!wifi_sdio_rx_commit(&rx, sizeof rx.data + 1, frame, NULL));
    rx.used = SIZE_MAX;
    assert(!wifi_sdio_rx_plan(&rx, 1, &logical, &physical, &address));
    assert(!wifi_sdio_rx_commit(&rx, 1, frame, NULL));
    puts("SDIO RX PASS: 20-frame burst, all 1465 splits, header tails, DMA padding, max frame, random chunks, malformed/bounds recovery");
}
