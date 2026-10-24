/* Tab5 microSD on ESP32-P4 SDMMC slot 0: dedicated 4-bit pins and polled
 * internal-DMA transfers, with no FreeRTOS dependency.  Slot 1 is shared with
 * the ESP32-C6 Wi-Fi transport through sdmmc_bus_lock(). */
#include <m5os/sdcard.h>
#include <m5os/bus.h>
#include <m5os/errno.h>
#include <m5os/hal_gpio.h>
#include <m5os/hal_time.h>
#include <m5os/kmalloc.h>
#include <m5os/task.h>
#include "esp32p4/sdmmc_shared.h"

#include "soc/sdmmc_struct.h"
#include "soc/sdmmc_reg.h"
#include "soc/sdmmc_pins.h"
#include "soc/gpio_sig_map.h"
#include "hal/sdmmc_ll.h"
#include "hal/gpio_ll.h"
#include "esp_cache.h"
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"

#define SLOT                 0u
#define BLOCK_BYTES          512u
#define DMA_BLOCKS           16u
#define DMA_BYTES            (DMA_BLOCKS * BLOCK_BYTES)
#define CACHE_LINE_BYTES     128u
#define INIT_TIMEOUT_MS      1500u
#define TRANSFER_TIMEOUT_US  3000000u
#define R1_ERROR_MASK        0xfdffe008u
#define OCR_VOLTAGE          0x00ff8000u
#define OCR_READY            0x80000000u
#define OCR_HIGH_CAPACITY    0x40000000u

static sdcard_status_t s = {
    .detail = "not probed",
    .transport = "native SDMMC slot 0",
};
static blockdev_t s_bd;
static uint16_t s_rca;
static sdmmc_desc_t s_desc[DMA_BLOCKS / 8] __attribute__((aligned(64)));
static void *s_dma_allocation;
static uint8_t *s_dma;

static uint32_t now_ms(void)
{
    return (uint32_t)(hal_time_now_us() / 1000u);
}

static void delay_ms(uint32_t milliseconds)
{
    /* rootfs_mount() runs on the bootstrap idle context after sched_init(),
     * before any other runnable task exists. Sleeping task 1 there would leave
     * the scheduler with no runnable task. Shell reprobes can sleep normally. */
    task_t *current = task_current();
    if (current && current->id != 1) task_sleep(milliseconds);
    else hal_delay_us(milliseconds * 1000u);
}

static uint32_t response_bits(const uint32_t response[4], unsigned start,
                              unsigned length)
{
    uint32_t mask = length == 32 ? UINT32_MAX : UINT32_MAX >> (32u - length);
    unsigned word = start / 32u;
    unsigned shift = start % 32u;
    uint32_t right = response[word] >> shift;
    uint32_t left = length + shift <= 32u ? 0u
                    : response[word + 1u] << (32u - shift);
    return (left | right) & mask;
}

static bool command(unsigned index, uint32_t argument, unsigned flags,
                    bool send_init, uint32_t response[4])
{
    bool ok = tab5_sdmmc_command_locked(SLOT, index, argument, flags,
                                        send_init, response);
    if (ok && response) {
        s.last_status = response[0];
        s.last_r1 = (uint8_t)response[0];
    }
    return ok;
}

static bool r1(unsigned index, uint32_t argument)
{
    uint32_t response[4];
    return command(index, argument,
                   TAB5_SDMMC_RSP_PRESENT | TAB5_SDMMC_RSP_CRC,
                   false, response) && !(response[0] & R1_ERROR_MASK);
}

static bool app_command(unsigned index, uint32_t argument, uint32_t response[4],
                        bool response_crc)
{
    if (!r1(55, (uint32_t)s_rca << 16)) return false;
    unsigned flags = TAB5_SDMMC_RSP_PRESENT;
    if (response_crc) flags |= TAB5_SDMMC_RSP_CRC;
    return command(index, argument, flags, false, response);
}

static void iomux_pin(unsigned pin, bool pullup)
{
    gpio_ll_func_sel(&GPIO, pin, SDMMC_SLOT0_FUNC);
    gpio_ll_input_enable(&GPIO, pin);
    gpio_ll_set_drive_capability(&GPIO, pin, GPIO_DRIVE_CAP_3);
    gpio_ll_pulldown_dis(&GPIO, pin);
    if (pullup) gpio_ll_pullup_en(&GPIO, pin);
    else gpio_ll_pullup_dis(&GPIO, pin);
}

static void setup_slot_pins_1bit(void)
{
    iomux_pin(SDMMC_SLOT0_IOMUX_PIN_NUM_CLK, false);
    iomux_pin(SDMMC_SLOT0_IOMUX_PIN_NUM_CMD, true);
    iomux_pin(SDMMC_SLOT0_IOMUX_PIN_NUM_D0, true);
    iomux_pin(SDMMC_SLOT0_IOMUX_PIN_NUM_D1, true);
    iomux_pin(SDMMC_SLOT0_IOMUX_PIN_NUM_D2, true);

    /* D3 low at CMD0 selects SPI mode.  Hold it high until ACMD6 has switched
     * both card and host to the 4-bit native bus. */
    unsigned d3 = SDMMC_SLOT0_IOMUX_PIN_NUM_D3;
    gpio_ll_func_sel(&GPIO, d3, PIN_FUNC_GPIO);
    gpio_ll_pullup_en(&GPIO, d3);
    gpio_ll_pulldown_dis(&GPIO, d3);
    gpio_ll_set_level(&GPIO, d3, 1);
    gpio_ll_output_enable(&GPIO, d3);

    esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ONE_INPUT,
                                   SD_CARD_INT_N_1_PAD_IN_IDX, false);
    esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ZERO_INPUT,
                                   SD_CARD_DETECT_N_1_PAD_IN_IDX, false);
    esp_rom_gpio_connect_in_signal(GPIO_MATRIX_CONST_ONE_INPUT,
                                   SD_CARD_WRITE_PRT_1_PAD_IN_IDX, true);
}

static bool decode_csd(const uint32_t csd[4], uint32_t *sectors,
                       bool *write_protected)
{
    unsigned version = response_bits(csd, 126, 2);
    uint64_t count;
    if (version == 1) {
        count = ((uint64_t)response_bits(csd, 48, 22) + 1u) * 1024u;
    } else if (version == 0) {
        unsigned read_bl_len = response_bits(csd, 80, 4);
        unsigned size_mult = response_bits(csd, 47, 3);
        uint64_t blocks = ((uint64_t)response_bits(csd, 62, 12) + 1u)
                          << (size_mult + 2u);
        if (read_bl_len > 31u) return false;
        uint64_t bytes = blocks << read_bl_len;
        count = bytes / BLOCK_BYTES;
    } else {
        return false;
    }
    if (!count || count > UINT32_MAX) return false;
    *sectors = (uint32_t)count;
    *write_protected = response_bits(csd, 12, 2) != 0;
    return true;
}

static bool ensure_dma_buffer(void)
{
    if (s_dma) return true;

    /* ESP32-P4 SDMMC can DMA to PSRAM.  Allocate only when the card is used so
     * the 8 KiB staging area does not consume scarce linked internal DRAM or
     * add any cost to a boot which never probes microSD. */
    s_dma_allocation = kmalloc_psram(DMA_BYTES + CACHE_LINE_BYTES - 1u);
    if (!s_dma_allocation) return false;
    s_dma = (uint8_t *)(((uintptr_t)s_dma_allocation + CACHE_LINE_BYTES - 1u) &
                        ~(uintptr_t)(CACHE_LINE_BYTES - 1u));
    return true;
}

static bool prepare_dma(size_t length)
{
    if (!s_dma || !length || length > DMA_BYTES ||
        (length & (BLOCK_BYTES - 1u)))
        return false;
    size_t sync_length = (length + CACHE_LINE_BYTES - 1u) &
                         ~(CACHE_LINE_BYTES - 1u);
    esp_cache_msync(s_dma, sync_length, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    for (size_t i = 0; i < sizeof s_desc; i++) ((uint8_t *)s_desc)[i] = 0;

    size_t remaining = length;
    uint8_t *buffer = s_dma;
    unsigned descriptor = 0;
    while (remaining) {
        size_t chunk = remaining > 4096u ? 4096u : remaining;
        if (descriptor >= sizeof s_desc / sizeof s_desc[0]) return false;
        sdmmc_desc_t *d = &s_desc[descriptor];
        d->second_address_chained = 1;
        d->owned_by_idmac = 1;
        d->first_descriptor = descriptor == 0;
        d->last_descriptor = remaining <= 4096u;
        d->disable_int_on_completion = remaining > 4096u;
        d->buffer1_size = chunk;
        d->buffer1_ptr = buffer;
        d->next_desc_ptr = remaining <= 4096u ? NULL : &s_desc[descriptor + 1u];
        buffer += chunk;
        remaining -= chunk;
        descriptor++;
    }
    esp_cache_msync(s_desc, sizeof s_desc, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    return true;
}

static bool transfer_locked(bool write, uint32_t lba, uint32_t blocks, void *data)
{
    size_t length = (size_t)blocks * BLOCK_BYTES;
    if (!blocks || blocks > DMA_BLOCKS) return false;
    if (write) {
        const uint8_t *source = data;
        for (size_t i = 0; i < length; i++) s_dma[i] = source[i];
    }
    if (!prepare_dma(length) ||
        !tab5_sdmmc_wait_idle_locked(500000u)) return false;

    sdmmc_ll_reset_fifo(&SDMMC);
    for (unsigned i = 0; i < 1000 && !sdmmc_ll_is_fifo_reset_done(&SDMMC); i++)
        esp_rom_delay_us(1);
    if (!sdmmc_ll_is_fifo_reset_done(&SDMMC)) return false;

    SDMMC.rintsts.val = 0xffffffffu;
    SDMMC.idsts.val = 0xffffffffu;
    sdmmc_ll_set_data_transfer_len(&SDMMC, length);
    sdmmc_ll_set_block_size(&SDMMC, BLOCK_BYTES);
    sdmmc_ll_set_desc_addr(&SDMMC, (uint32_t)(uintptr_t)&s_desc[0]);
    sdmmc_ll_enable_dma(&SDMMC, true);
    sdmmc_ll_poll_demand(&SDMMC);

    unsigned index = write ? (blocks == 1 ? 24u : 25u)
                           : (blocks == 1 ? 17u : 18u);
    uint32_t argument = s.high_capacity ? lba : lba * BLOCK_BYTES;
    SDMMC.cmdarg = argument;
    sdmmc_hw_cmd_t command_reg = { 0 };
    command_reg.cmd_index = index;
    command_reg.card_num = SLOT;
    command_reg.response_expect = 1;
    command_reg.check_response_crc = 1;
    command_reg.data_expected = 1;
    command_reg.rw = write;
    command_reg.send_auto_stop = blocks > 1;
    command_reg.wait_complete = 1;
    command_reg.use_hold_reg = 1;
    command_reg.start_command = 1;
    SDMMC.cmd = command_reg;

    const uint32_t error_mask = SDMMC_LL_EVENT_RTO | SDMMC_LL_EVENT_DTO |
        SDMMC_LL_EVENT_HTO | SDMMC_LL_EVENT_RCRC | SDMMC_LL_EVENT_DCRC |
        SDMMC_LL_EVENT_RESP_ERR | SDMMC_LL_EVENT_HLE | SDMMC_LL_EVENT_FRUN |
        SDMMC_LL_EVENT_SBE | SDMMC_LL_EVENT_EBE;
    const uint32_t dma_error_mask = SDMMC_IDMAC_INTMASK_CES |
        SDMMC_IDMAC_INTMASK_DU | SDMMC_IDMAC_INTMASK_FBE;
    bool ok = false;
    for (uint32_t elapsed = 0; elapsed < TRANSFER_TIMEOUT_US; elapsed++) {
        uint32_t status = SDMMC.rintsts.val;
        uint32_t dma_status = SDMMC.idsts.val;
        if ((status & error_mask) || (dma_status & dma_error_mask)) break;
        if ((status & SDMMC_LL_EVENT_CMD_DONE) &&
            (status & SDMMC_LL_EVENT_DATA_OVER)) {
            s.last_status = SDMMC.resp[0];
            s.last_r1 = (uint8_t)s.last_status;
            ok = !(s.last_status & R1_ERROR_MASK);
            break;
        }
        esp_rom_delay_us(1);
    }
    sdmmc_ll_stop_dma(&SDMMC);
    SDMMC.rintsts.val = 0xffffffffu;
    SDMMC.idsts.val = 0xffffffffu;
    if (ok && write) ok = tab5_sdmmc_wait_idle_locked(TRANSFER_TIMEOUT_US);
    if (ok && !write) {
        esp_cache_msync(s_dma, (length + CACHE_LINE_BYTES - 1u) &
                              ~(CACHE_LINE_BYTES - 1u),
                        ESP_CACHE_MSYNC_FLAG_DIR_M2C);
        uint8_t *destination = data;
        for (size_t i = 0; i < length; i++) destination[i] = s_dma[i];
    }
    return ok;
}

static int transfer(bool write, uint32_t lba, uint32_t count, void *data)
{
    if (!s.ready) return ERR_NODEV;
    if (!data || !count || lba >= s.sectors || count > s.sectors - lba)
        return ERR_INVAL;
    if (write && s.write_protected) return ERR_PERM;

    uint8_t *bytes = data;
    int rc = ERR_OK;
    while (count) {
        uint32_t chunk = count > DMA_BLOCKS ? DMA_BLOCKS : count;
        sdmmc_bus_lock();
        if (!transfer_locked(write, lba, chunk, bytes)) {
            s.ready = false;
            s.detail = write ? "SDMMC write error; unmount then probe again"
                             : "SDMMC read error; unmount then probe again";
            rc = ERR_IO;
            sdmmc_bus_unlock();
            break;
        }
        sdmmc_bus_unlock();
        lba += chunk;
        count -= chunk;
        bytes += (size_t)chunk * BLOCK_BYTES;
    }
    return rc;
}

static int read_blocks(blockdev_t *device, uint32_t lba, uint32_t count, void *data)
{
    (void)device;
    return transfer(false, lba, count, data);
}

static int write_blocks(blockdev_t *device, uint32_t lba, uint32_t count,
                        const void *data)
{
    (void)device;
    return transfer(true, lba, count, (void *)data);
}

blockdev_t *sdcard_probe(void)
{
    sdmmc_bus_lock();
    s = (sdcard_status_t){
        .probed = true,
        .detail = "slot power/controller init failed",
        .transport = "native SDMMC slot 0",
    };
    s_rca = 0;
    bool ok = false;
    bool acmd41_clock_forced = false;

    if (!ensure_dma_buffer()) {
        s.detail = "no PSRAM for SDMMC DMA buffer";
        goto out;
    }
    if (!sdcard_spi_power_init() || !tab5_sdmmc_host_init_locked()) goto out;
    setup_slot_pins_1bit();
    sdmmc_ll_set_card_width(&SDMMC, SLOT, SD_BUS_WIDTH_1_BIT);
    if (!tab5_sdmmc_set_clock_locked(SLOT, 400)) goto out;
    delay_ms(2);

    s.detail = "no response or card fault (no card-detect pin)";
    command(0, 0, 0, true, NULL);
    delay_ms(10);
    uint32_t response[4];
    bool version2 = command(8, 0x1aau,
                            TAB5_SDMMC_RSP_PRESENT | TAB5_SDMMC_RSP_CRC,
                            false, response) && (response[0] & 0xfffu) == 0x1aau;

    s.detail = "ACMD41 voltage negotiation timed out";
    /* SD Physical Layer 4.4 requires a continuous card clock from ACMD41
     * until power-up completes.  The normal low-power setting gates cclk
     * between commands, which leaves some SDSC cards permanently not-ready. */
    if (!tab5_sdmmc_set_cclk_always_on_locked(SLOT, true)) goto out;
    acmd41_clock_forced = true;
    uint32_t start = now_ms();
    uint32_t ocr = 0;
    do {
        if (!app_command(41, OCR_VOLTAGE | (version2 ? OCR_HIGH_CAPACITY : 0),
                         response, false)) goto out;
        ocr = response[0];
        if (ocr & OCR_READY) break;
        delay_ms(10);
    } while (now_ms() - start < INIT_TIMEOUT_MS);
    if (!(ocr & OCR_READY) || !(ocr & OCR_VOLTAGE)) goto out;
    if (!tab5_sdmmc_set_cclk_always_on_locked(SLOT, false)) goto out;
    acmd41_clock_forced = false;
    s.high_capacity = (ocr & OCR_HIGH_CAPACITY) != 0;

    s.detail = "CID/RCA/CSD initialization failed";
    if (!command(2, 0, TAB5_SDMMC_RSP_PRESENT | TAB5_SDMMC_RSP_CRC |
                 TAB5_SDMMC_RSP_LONG, false, response)) goto out;
    if (!command(3, 0, TAB5_SDMMC_RSP_PRESENT | TAB5_SDMMC_RSP_CRC,
                 false, response)) goto out;
    s_rca = (uint16_t)(response[0] >> 16);
    if (!s_rca || !command(9, (uint32_t)s_rca << 16,
                           TAB5_SDMMC_RSP_PRESENT | TAB5_SDMMC_RSP_CRC |
                           TAB5_SDMMC_RSP_LONG, false, response) ||
        !decode_csd(response, &s.sectors, &s.write_protected)) goto out;
    if (!r1(7, (uint32_t)s_rca << 16)) goto out;
    if (!s.high_capacity && !r1(16, BLOCK_BYTES)) goto out;

    s.detail = "4-bit bus switch failed";
    if (!app_command(6, 2, response, true) ||
        (response[0] & R1_ERROR_MASK)) goto out;
    iomux_pin(SDMMC_SLOT0_IOMUX_PIN_NUM_D3, true);
    sdmmc_ll_set_card_width(&SDMMC, SLOT, SD_BUS_WIDTH_4_BIT);
    if (!tab5_sdmmc_set_clock_locked(SLOT, 20000)) goto out;

    s.bus_width = 4;
    s.clock_khz = 20000;
    s.ready = true;
    s.detail = s.write_protected ? "ready (SDMMC, card write-protected)"
                                 : "ready (SDMMC, read/write)";
    ok = true;
out:
    if (acmd41_clock_forced)
        tab5_sdmmc_set_cclk_always_on_locked(SLOT, false);
    sdmmc_bus_unlock();
    if (!ok) return NULL;
    s_bd = (blockdev_t){
        .name = "sd0",
        .block_size = BLOCK_BYTES,
        .block_count = s.sectors,
        .read = read_blocks,
        .write = write_blocks,
    };
    return &s_bd;
}

void sdcard_status(sdcard_status_t *out)
{
    sdmmc_bus_lock();
    *out = s;
    sdmmc_bus_unlock();
}

