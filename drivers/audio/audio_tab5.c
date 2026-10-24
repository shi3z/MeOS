/* Bare-metal Tab5 ES8388 + I2S0/AHB-DMA TX2. No IDF driver/FreeRTOS locks.
 * Wiring/register reference: M5Unified _speaker_enabled_cb_tab5 and Tab5 BSP.
 * Reserve AHB TX2 for audio; no other MeOS driver uses AHB GDMA. Never reset a
 * running DMA group: camera/display use independent DMA hardware. */
#include <m5os/board_config.h>
#if M5OS_BOARD_TAB5
#include <m5os/audio.h>
#include <m5os/hal_i2c.h>
#include <m5os/hal_gpio.h>
#include <m5os/arch.h>
#include <m5os/task.h>
#include <m5os/mutex.h>
#include <m5os/peripheral.h>
#include <m5os/kmalloc.h>
#include <m5os/kprintf.h>
#include "hal/i2s_ll.h"
#include "hal/ahb_dma_ll.h"
#include "hal/dma_types.h"
#include "soc/gpio_sig_map.h"
#include "soc/gdma_channel.h"
#include "esp_rom_gpio.h"
#include "esp_cache.h"
#include <string.h>
#include "sdkconfig.h"
#define AUDIO_ALIGN CONFIG_CACHE_L2_CACHE_LINE_SIZE
#define CH 2
#define BLOCKS 8
#define FRAMES 512
typedef struct { dma_descriptor_t d; uint8_t pad[AUDIO_ALIGN-sizeof(dma_descriptor_t)]; } __attribute__((aligned(AUDIO_ALIGN))) cache_desc_t;
_Static_assert(sizeof(cache_desc_t)==AUDIO_ALIGN,"one descriptor per hardware cache line");
_Static_assert((FRAMES*4)%AUDIO_ALIGN==0,"PCM blocks must span complete cache lines");
static cache_desc_t s_desc[BLOCKS];
static int16_t *s_pcm;
static void *s_allocation;
static mutex_t s_playback_lock = MUTEX_INIT;
static const char s_resource_owner;
static const peripheral_t s_resources[] = { PERIPHERAL_I2S0, PERIPHERAL_AHB_DMA_TX2 };
static bool s_hw, s_busy, s_task, s_claimed;
static uint32_t s_owner, s_blocks, s_errors;
static unsigned s_next;
static audio_render_t s_render;
static void *s_arg;
static bool reg(uint8_t r, uint8_t v) { return hal_i2c_sys_write_reg(0x10, r, v); }
static bool amp(bool on) { return hal_i2c_sys_update_reg(0x43, 5, 2, on ? 2 : 0); }
static void pin(int gpio, int signal)
{ hal_gpio_config_output(gpio, 0); esp_rom_gpio_connect_out_signal(gpio, signal, false, false); }
static bool hardware(void)
{
    if (s_hw) return true;
    uint32_t irq = arch_irq_disable();
    bool was_on = gdma_ll_is_bus_clock_enabled(0);
    _gdma_ll_enable_bus_clock(0, true);
    if (!was_on) _gdma_ll_reset_register(0);
    if (was_on && (!ahb_dma_ll_tx_is_desc_fsm_idle(&AHB_DMA, CH) ||
        AHB_DMA.channel[CH].out.out_peri_sel.peri_out_sel_chn != 63)) {
        arch_irq_restore(irq); kputs("audio: AHB TX2 busy\n"); return false;
    }
    ahb_dma_ll_force_enable_reg_clock(&AHB_DMA, true);
    ahb_dma_ll_set_default_memory_range(&AHB_DMA);
    int __DECLARE_RCC_ATOMIC_ENV = 0;
    i2s_ll_enable_bus_clock(0, true); i2s_ll_reset_register(0);
    _i2s_ll_tx_enable_clock(&I2S0); _i2s_ll_mclk_bind_to_tx_clk(&I2S0);
    _i2s_ll_tx_clk_set_src(&I2S0, I2S_CLK_SRC_XTAL);
    hal_utils_clk_div_t div = { .integer = 13, .numerator = 1, .denominator = 48 };
    _i2s_ll_tx_set_mclk(&I2S0, &div); /* 40MHz / (13+1/48) = 24000*128 */
    i2s_ll_tx_set_bck_div_num(&I2S0, 4);
    i2s_ll_tx_reset(&I2S0); i2s_ll_tx_set_slave_mod(&I2S0, false);
    i2s_ll_tx_set_sample_bit(&I2S0, 16, 16); i2s_ll_tx_set_half_sample_bit(&I2S0, 16);
    i2s_ll_tx_enable_msb_shift(&I2S0, true); i2s_ll_tx_set_ws_width(&I2S0, 16);
    i2s_ll_tx_enable_mono_mode(&I2S0, false); i2s_ll_tx_select_std_slot(&I2S0, I2S_STD_SLOT_BOTH);
    i2s_ll_tx_set_skip_mask(&I2S0, false); i2s_ll_tx_set_ws_idle_pol(&I2S0, false);
    i2s_ll_tx_set_bit_order(&I2S0, false); i2s_ll_tx_enable_left_align(&I2S0, false);
    i2s_ll_tx_enable_big_endian(&I2S0, false); i2s_ll_tx_enable_std(&I2S0);
    I2S0.tx_conf.tx_stop_en = 0;
    ahb_dma_ll_tx_reset_channel(&AHB_DMA, CH);
    ahb_dma_ll_tx_enable_interrupt(&AHB_DMA, CH, GDMA_LL_TX_EVENT_MASK, false);
    ahb_dma_ll_tx_enable_owner_check(&AHB_DMA, CH, true);
    ahb_dma_ll_tx_enable_auto_write_back(&AHB_DMA, CH, true);
    ahb_dma_ll_tx_set_eof_mode(&AHB_DMA, CH, 1);
    ahb_dma_ll_tx_enable_descriptor_burst(&AHB_DMA, CH, true);
    ahb_dma_ll_tx_enable_data_burst(&AHB_DMA, CH, false);
    ahb_dma_ll_tx_connect_to_periph(&AHB_DMA, CH, GDMA_TRIG_PERIPH_I2S, SOC_GDMA_TRIG_PERIPH_I2S0);
    s_hw = true; /* teardown can now stop our configured DMA after app termination */
    arch_irq_restore(irq);
    pin(30, I2S0_MCLK_PAD_OUT_IDX); pin(27, I2S0_O_BCK_PAD_OUT_IDX);
    pin(29, I2S0_O_WS_PAD_OUT_IDX); pin(26, I2S0_O_SD_PAD_OUT_IDX);
    s_hw = true; return true;
}
static void start_dma(void)
{
    s_next = 0;
    memset(s_pcm, 0, BLOCKS * FRAMES * 4);
    for (unsigned i = 0; i < BLOCKS; i++) {
        s_desc[i].d = (dma_descriptor_t){ .dw0 = { .size = FRAMES * 4,
            .length = FRAMES * 4, .suc_eof = 1, .owner = 1 },
            .buffer = s_pcm + i * FRAMES * 2, .next = &s_desc[(i + 1) % BLOCKS].d };
    }
    esp_cache_msync(s_pcm, BLOCKS*FRAMES*4, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    esp_cache_msync(s_desc, sizeof s_desc, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    __asm__ volatile("fence iorw,iorw" ::: "memory");
    ahb_dma_ll_tx_clear_interrupt_status(&AHB_DMA, CH, GDMA_LL_TX_EVENT_MASK);
    i2s_ll_tx_reset_fifo(&I2S0);
    ahb_dma_ll_tx_set_desc_addr(&AHB_DMA, CH, (uint32_t)s_desc);
    ahb_dma_ll_tx_start(&AHB_DMA, CH); i2s_ll_tx_start(&I2S0);
}
static void worker(void *arg)
{
    (void)arg;
    for (;;) {
        task_sleep(5);
        mutex_lock(&s_playback_lock);
        if (!s_busy) { mutex_unlock(&s_playback_lock); continue; }
        uint32_t status = ahb_dma_ll_tx_get_interrupt_status(&AHB_DMA, CH, true);
        if (status & GDMA_LL_EVENT_TX_DESC_ERROR) {
            s_errors++; i2s_ll_tx_stop(&I2S0); ahb_dma_ll_tx_stop(&AHB_DMA, CH);
            ahb_dma_ll_tx_reset_channel(&AHB_DMA, CH); start_dma(); mutex_unlock(&s_playback_lock); continue;
        }
        for (unsigned n = 0; n < BLOCKS; n++) {
            unsigned i = s_next;
            esp_cache_msync(&s_desc[i], AUDIO_ALIGN, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
            volatile dma_descriptor_t *d = &s_desc[i].d;
            if (d->dw0.owner) break;
            s_render(s_pcm + i * FRAMES * 2, FRAMES, s_arg);
            __asm__ volatile("fence iorw,iorw" ::: "memory");
            esp_cache_msync(s_pcm + i*FRAMES*2, FRAMES*4, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
            d->dw0.owner = 1;
            esp_cache_msync(&s_desc[i], AUDIO_ALIGN, ESP_CACHE_MSYNC_FLAG_DIR_C2M); s_blocks++; s_next = (i + 1) % BLOCKS;
        }
        mutex_unlock(&s_playback_lock);
    }
}
static void release_hardware(void)
{
    if (s_hw) {
        i2s_ll_tx_stop(&I2S0); ahb_dma_ll_tx_stop(&AHB_DMA, CH);
        ahb_dma_ll_tx_reset_channel(&AHB_DMA, CH);
        ahb_dma_ll_tx_disconnect_from_periph(&AHB_DMA, CH);
        s_hw = false;
    }
    peripheral_release_owner(&s_resource_owner);
}
bool audio_open(uint32_t owner, audio_render_t render, void *arg)
{
    uint32_t irq = arch_irq_disable();
    if (s_claimed || !owner || !render) { arch_irq_restore(irq); kprintf("audio: open rejected claimed=%u owner=%u\n",s_claimed,owner); return false; }
    if (!peripheral_claim(s_resources, 2, &s_resource_owner)) {
        arch_irq_restore(irq); kputs("audio: I2S0/DMA reserved\n"); return false;
    }
    s_claimed = true; s_owner = owner;
    arch_irq_restore(irq);
    unsigned stage=1;
    if (!hardware()) goto fail;
    stage=2; if (!amp(false)) goto fail;
    /* Codec slave, 16-bit Philips I2S, MCLK/LRCK=128, DAC-only mixers.
     * Keep the ADC off and digital gain at -18dB for the initial bring-up. */
    static const uint8_t setup[][2] = {
        {25,0x24}, {0,0x80}, {0,0}, {0,0x0e}, {1,0}, {2,0x0a}, {3,0xff},
        {4,0xc0}, {5,0}, {6,0}, {7,0x7c}, {8,0}, {23,0x18}, {24,0},
        {25,0x24}, {26,36}, {27,36}, {28,8}, {29,0}, {38,0},
        {39,0x90}, {42,0x90}, {43,8}, {45,0}, {46,0x1e}, {47,0x1e},
        {48,0x1e}, {49,0x1e}, {4,0x3c}
    };
    for (unsigned i = 0; i < sizeof setup / sizeof setup[0]; i++)
        { stage=100+setup[i][0]; if (!reg(setup[i][0], setup[i][1])) goto fail; }
    stage=3;
    if (!s_task) {
        if (task_create_for_app("audio", worker, NULL, 3, 4096, NULL) < 0) goto fail;
        s_task = true;
    }
    stage=4;
    s_allocation = kmalloc_internal(BLOCKS * FRAMES * 4 + AUDIO_ALIGN-1);
    if (!s_allocation) goto fail;
    s_pcm = (int16_t *)(((uintptr_t)s_allocation+AUDIO_ALIGN-1)&~(uintptr_t)(AUDIO_ALIGN-1));
    s_owner = owner; s_render = render; s_arg = arg; s_blocks = s_errors = 0;
    start_dma(); s_busy = true;
    task_sleep(30);
    if (!reg(25,0x20) || !amp(true)) { audio_close(owner); return false; }
    kputs("audio: ES8388 24000Hz stereo, DMA TX2, -18dB\n"); return true;
fail:
    kprintf("audio: initialization failed at stage %u\n",stage);
    if (stage > 1) { amp(false); reg(25,0x24); reg(4,0xc0); reg(2,0xff); }
    irq = arch_irq_disable();
    release_hardware(); s_claimed = false; s_owner = 0;
    arch_irq_restore(irq); return false;
}
void audio_close(uint32_t owner)
{
    mutex_lock(&s_playback_lock);
    if (!s_claimed || s_owner != owner) { mutex_unlock(&s_playback_lock); return; }
    amp(false); reg(25,0x24); task_sleep(10); reg(4,0xc0); reg(2,0xff);
    uint32_t irq = arch_irq_disable();
    s_busy = false;
    release_hardware();
    s_render = NULL;
    void *p = s_allocation; s_allocation = NULL; s_pcm = NULL; s_owner = 0; s_claimed = false;
    arch_irq_restore(irq); kfree(p);
    mutex_unlock(&s_playback_lock);
    kprintf("audio: stopped, %u blocks, %u underruns/errors\n", s_blocks, s_errors);
}
static void tone(int16_t *out, unsigned frames, void *arg)
{
    uint32_t *phase = arg;
    for (unsigned i = 0; i < frames; i++) {
        *phase = (*phase + 440u) % AUDIO_RATE;
        int v = (int)(*phase * 2048u / AUDIO_RATE);
        if (v >= 1024) v = 2048 - v;
        v -= 512; out[i*2] = out[i*2+1] = v;
    }
}
int audio_command(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "test")) {
        uint32_t phase = 0;
        if (!audio_open(UINT32_MAX, tone, &phase)) { kputs("audio: busy/unavailable\n"); return 1; }
        task_sleep(1200); audio_close(UINT32_MAX); return 0;
    }
    kprintf("audio: %s, %u blocks, %u errors; usage: audio test\n", s_busy ? "playing" : "idle", s_blocks, s_errors);
    return 0;
}
#endif
