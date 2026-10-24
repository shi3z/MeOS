/* Shared FreeRTOS-free ESP32-P4 SDMMC controller setup for Tab5 slot 0
 * (microSD) and slot 1 (ESP32-C6 SDIO). */
#include "sdmmc_shared.h"

#include "soc/sdmmc_struct.h"
#include "hal/sdmmc_ll.h"
#include "esp_private/esp_clk_tree_common.h"
#include "esp_rom_sys.h"

/* Satisfy the low-level clock macro's atomic-environment check. */
static int __DECLARE_RCC_ATOMIC_ENV;
static bool s_initialized;

bool tab5_sdmmc_wait_idle_locked(uint32_t timeout_us)
{
    for (uint32_t elapsed = 0; elapsed < timeout_us; elapsed += 2u) {
        if (!SDMMC.cmd.start_command && !sdmmc_ll_is_card_data_busy(&SDMMC))
            return true;
        esp_rom_delay_us(2);
    }
    return false;
}

static bool reset_done(void)
{
    return sdmmc_ll_is_controller_reset_done(&SDMMC) &&
           sdmmc_ll_is_fifo_reset_done(&SDMMC) &&
           sdmmc_ll_is_dma_reset_done(&SDMMC);
}

bool tab5_sdmmc_host_init_locked(void)
{
    if (s_initialized) return true;

    sdmmc_ll_enable_bus_clock(&SDMMC, true);
    sdmmc_ll_reset_register(&SDMMC);
    esp_clk_tree_enable_src((soc_module_clk_t)SDMMC_CLK_SRC_PLL160M, true);

    /* A fixed 40 MHz controller clock lets slot 0 run at 40 MHz while slot 1
     * uses its own /2 divider for the existing 20 MHz ESP32-C6 link. */
    sdmmc_ll_set_clock_div(&SDMMC, 4);
    sdmmc_ll_select_clk_source(&SDMMC, SDMMC_CLK_SRC_PLL160M);
    sdmmc_ll_init_phase_delay(&SDMMC);
    esp_rom_delay_us(20);

    sdmmc_ll_reset_controller(&SDMMC);
    sdmmc_ll_reset_fifo(&SDMMC);
    sdmmc_ll_reset_dma(&SDMMC);
    for (unsigned i = 0; i < 2000 && !reset_done(); i++)
        esp_rom_delay_us(1);
    if (!reset_done()) return false;

    sdmmc_ll_clear_interrupt(&SDMMC, 0xffffffffu);
    sdmmc_ll_enable_interrupt(&SDMMC, 0xffffffffu, false);
    sdmmc_ll_enable_global_interrupt(&SDMMC, false);
    sdmmc_ll_enable_busy_clear_interrupt(&SDMMC, false);
    sdmmc_ll_init_dma(&SDMMC);
    sdmmc_ll_set_response_timeout(&SDMMC, 255);
    sdmmc_ll_set_data_timeout(&SDMMC, 0x00ffffffu);
    s_initialized = true;
    return true;
}

static bool update_clock(unsigned slot)
{
    sdmmc_hw_cmd_t command = { 0 };
    command.card_num = slot;
    command.update_clk_reg = 1;
    command.wait_complete = 1;
    command.use_hold_reg = 1;
    command.start_command = 1;

    for (unsigned tries = 0; tries < 200; tries++) {
        SDMMC.rintsts.val = 0xffffffffu;
        SDMMC.cmd = command;
        for (unsigned poll = 0; poll < 20000; poll++) {
            if (SDMMC.rintsts.hle) break;
            if (!SDMMC.cmd.start_command) {
                SDMMC.rintsts.val = 0xffffffffu;
                return true;
            }
            esp_rom_delay_us(1);
        }
        esp_rom_delay_us(10);
    }
    return false;
}

bool tab5_sdmmc_set_cclk_always_on_locked(unsigned slot, bool always_on)
{
    if (!s_initialized || slot > 1) return false;
    sdmmc_ll_enable_card_clock_low_power(&SDMMC, slot, !always_on);
    return update_clock(slot);
}

bool tab5_sdmmc_set_clock_locked(unsigned slot, uint32_t frequency_khz)
{
    if (!s_initialized || slot > 1 || !frequency_khz || frequency_khz > 40000u)
        return false;
    sdmmc_ll_enable_card_clock(&SDMMC, slot, false);
    if (!update_clock(slot)) return false;

    /* f_slot = 40 MHz for divider 0, otherwise 40 MHz / (2 * divider). */
    uint32_t divider = frequency_khz >= 40000u ? 0u
                       : (40000u + 2u * frequency_khz - 1u) /
                         (2u * frequency_khz);
    if (divider > 255u) divider = 255u;
    sdmmc_ll_set_card_clock_div(&SDMMC, slot, divider);
    sdmmc_ll_init_phase_delay(&SDMMC);
    if (!update_clock(slot)) return false;
    sdmmc_ll_enable_card_clock(&SDMMC, slot, true);
    sdmmc_ll_enable_card_clock_low_power(&SDMMC, slot, true);
    return update_clock(slot);
}

bool tab5_sdmmc_command_locked(unsigned slot, unsigned command_index,
                               uint32_t argument, unsigned response_flags,
                               bool send_init, uint32_t response[4])
{
    if (!s_initialized || slot > 1 || command_index > 63 ||
        !tab5_sdmmc_wait_idle_locked(200000u)) return false;

    SDMMC.rintsts.val = 0xffffffffu;
    SDMMC.cmdarg = argument;
    sdmmc_hw_cmd_t command = { 0 };
    command.cmd_index = command_index;
    command.card_num = slot;
    command.response_expect = (response_flags & TAB5_SDMMC_RSP_PRESENT) != 0;
    command.check_response_crc = (response_flags & TAB5_SDMMC_RSP_CRC) != 0;
    command.response_long = (response_flags & TAB5_SDMMC_RSP_LONG) != 0;
    command.send_init = send_init;
    command.wait_complete = !send_init;
    command.use_hold_reg = 1;
    command.start_command = 1;
    SDMMC.cmd = command;

    for (uint32_t elapsed = 0; elapsed < 200000u; elapsed++) {
        uint32_t status = SDMMC.rintsts.val;
        if (status & SDMMC_LL_EVENT_HLE) {
            break;
        }
        if (status & SDMMC_LL_EVENT_CMD_DONE) {
            bool bad = (status & (SDMMC_LL_EVENT_RTO | SDMMC_LL_EVENT_RESP_ERR)) != 0;
            if ((response_flags & TAB5_SDMMC_RSP_CRC) &&
                (status & SDMMC_LL_EVENT_RCRC)) bad = true;
            if (!bad && response) {
                response[0] = SDMMC.resp[0];
                response[1] = SDMMC.resp[1];
                response[2] = SDMMC.resp[2];
                response[3] = SDMMC.resp[3];
            }
            SDMMC.rintsts.val = 0xffffffffu;
            return !bad;
        }
        esp_rom_delay_us(1);
    }
    SDMMC.rintsts.val = 0xffffffffu;
    return false;
}

