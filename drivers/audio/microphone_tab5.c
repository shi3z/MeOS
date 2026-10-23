/* Explicit diagnostic RX, exclusive I2S0 + AHB DMA RX2. No FreeRTOS APIs. */
#include <m5os/board_config.h>
#if M5OS_BOARD_TAB5
#include <m5os/microphone.h>
#include <m5os/arch.h>
#include <m5os/peripheral.h>
#include <m5os/mutex.h>
#include <m5os/hal_gpio.h>
#include <m5os/hal_i2c.h>
#include <m5os/hal_time.h>
#include <m5os/kmalloc.h>
#include <m5os/app.h>
#include <m5os/kprintf.h>
#include <m5os/errno.h>
#include "mic_internal.h"
#include "hal/i2s_ll.h"
#include "hal/ahb_dma_ll.h"
#include "hal/dma_types.h"
#include "soc/gpio_sig_map.h"
#include "soc/gdma_channel.h"
#include "esp_rom_gpio.h"
#include "esp_cache.h"
#include <string.h>
#define CH 2
#define BLOCKS 8
#define RING_BYTES (BLOCKS * MIC_BLOCK_BYTES)
typedef struct { dma_descriptor_t d; uint8_t pad[64-sizeof(dma_descriptor_t)]; }
    __attribute__((aligned(64))) mic_desc_t;
static mic_desc_t s_desc[BLOCKS];
static mutex_t s_lock = MUTEX_INIT;
static const char s_token;
static uint32_t s_owner, s_errors, s_blocks;
static bool s_hw, s_codec, s_stop_attempted, s_powerdown_ok = true;
static void *s_allocation;
static int16_t *s_pcm;
static mic_levels_t s_levels;
static void pin(int gpio, int signal)
{ hal_gpio_config_output(gpio,0); esp_rom_gpio_connect_out_signal(gpio,signal,false,false); }
static bool configure(void)
{
    uint32_t irq = arch_irq_disable();
    bool was_on = gdma_ll_is_bus_clock_enabled(0);
    _gdma_ll_enable_bus_clock(0,true);
    if (!was_on) _gdma_ll_reset_register(0);
    if (was_on && (!ahb_dma_ll_rx_is_desc_fsm_idle(&AHB_DMA,CH) ||
        AHB_DMA.channel[CH].in.in_peri_sel.peri_in_sel_chn != 63)) {
        arch_irq_restore(irq); return false;
    }
    s_hw = true;
    ahb_dma_ll_force_enable_reg_clock(&AHB_DMA,true);
    ahb_dma_ll_set_default_memory_range(&AHB_DMA);
    int __DECLARE_RCC_ATOMIC_ENV = 0;
    i2s_ll_enable_bus_clock(0,true); i2s_ll_reset_register(0);
    _i2s_ll_rx_enable_clock(&I2S0); _i2s_ll_mclk_bind_to_rx_clk(&I2S0);
    _i2s_ll_rx_clk_set_src(&I2S0,I2S_CLK_SRC_XTAL);
    /* 40MHz / (3 + 49/192) = 12.288MHz, /4 = 3.072MHz BCLK (4x16 slots). */
    hal_utils_clk_div_t div = {.integer=3,.numerator=49,.denominator=192};
    _i2s_ll_rx_set_mclk(&I2S0,&div); i2s_ll_rx_set_bck_div_num(&I2S0,4);
    i2s_ll_rx_reset(&I2S0); i2s_ll_rx_set_slave_mod(&I2S0,false);
    i2s_ll_rx_set_sample_bit(&I2S0,16,16); i2s_ll_rx_enable_mono_mode(&I2S0,false);
    i2s_ll_rx_enable_msb_shift(&I2S0,true); i2s_ll_rx_set_ws_width(&I2S0,32);
    i2s_ll_rx_set_ws_idle_pol(&I2S0,false); i2s_ll_rx_set_chan_num(&I2S0,MIC_SLOTS);
    i2s_ll_rx_set_active_chan_mask(&I2S0,0x0f); i2s_ll_rx_set_half_sample_bit(&I2S0,32);
    i2s_ll_rx_set_bit_order(&I2S0,false); i2s_ll_rx_enable_left_align(&I2S0,false);
    i2s_ll_rx_enable_big_endian(&I2S0,false); i2s_ll_rx_enable_tdm(&I2S0);
    i2s_ll_rx_set_eof_num(&I2S0,MIC_BLOCK_BYTES);
    ahb_dma_ll_rx_reset_channel(&AHB_DMA,CH);
    ahb_dma_ll_rx_enable_interrupt(&AHB_DMA,CH,GDMA_LL_RX_EVENT_MASK,false);
    ahb_dma_ll_rx_enable_owner_check(&AHB_DMA,CH,true);
    ahb_dma_ll_rx_enable_descriptor_burst(&AHB_DMA,CH,true);
    ahb_dma_ll_rx_enable_data_burst(&AHB_DMA,CH,false);
    ahb_dma_ll_rx_connect_to_periph(&AHB_DMA,CH,GDMA_TRIG_PERIPH_I2S,SOC_GDMA_TRIG_PERIPH_I2S0);
    arch_irq_restore(irq);
    pin(30,I2S0_MCLK_PAD_OUT_IDX); pin(27,I2S0_I_BCK_PAD_OUT_IDX); pin(29,I2S0_I_WS_PAD_OUT_IDX);
    hal_gpio_config_input_pullup(28);
    esp_rom_gpio_connect_in_signal(28,I2S0_I_SD_PAD_IN_IDX,false);
    return true;
}
static void stop(void) /* session lock held; also handles partial initialization */
{
    if (s_codec) { s_stop_attempted = true; s_powerdown_ok = mic_codec_stop(); s_codec = false; }
    uint32_t irq = arch_irq_disable();
    if (s_hw) {
        i2s_ll_rx_stop(&I2S0); ahb_dma_ll_rx_stop(&AHB_DMA,CH);
        ahb_dma_ll_rx_reset_channel(&AHB_DMA,CH);
        ahb_dma_ll_rx_disconnect_from_periph(&AHB_DMA,CH);
        int __DECLARE_RCC_ATOMIC_ENV = 0;
        i2s_ll_rx_disable_clock(&I2S0); s_hw = false;
    }
    void *allocation = s_allocation;
    s_allocation = NULL; s_pcm = NULL; s_owner = 0;
    peripheral_release_owner(&s_token);
    arch_irq_restore(irq);
    if (allocation) { memset(allocation,0,RING_BYTES+63); kfree(allocation); }
}
void microphone_app_cleanup(uint32_t app_id)
{
    mutex_lock(&s_lock);
    if (s_owner == app_id) stop();
    mutex_unlock(&s_lock);
}
static void summary(void)
{
    kprintf("mic: %u frames, %u DMA blocks, %u errors; power-down %s\n",
        s_levels.frames,s_blocks,s_errors,!s_stop_attempted ? "not started" : s_powerdown_ok ? "writes acknowledged" : "unconfirmed");
    for (unsigned c=0;c<MIC_SLOTS;c++) kprintf("slot %u: peak %u, mean-abs %u, clipped %u\n",c,
        s_levels.peak[c],s_levels.frames ? (unsigned)(s_levels.absolute_sum[c]/s_levels.frames) : 0,
        s_levels.clipped[c]);
}
static int capture(unsigned seconds)
{
    static const peripheral_t resources[] = {PERIPHERAL_I2S0,PERIPHERAL_AHB_DMA_RX2};
    uint32_t irq = arch_irq_disable();
    if (!peripheral_claim(resources,2,&s_token)) { arch_irq_restore(irq); return ERR_BUSY; }
    task_t *task = task_current();
    s_owner = task && task->app ? task->app->app_id : UINT32_MAX;
    arch_irq_restore(irq);
    s_levels = (mic_levels_t){0}; s_errors = s_blocks = 0; s_powerdown_ok = true; s_stop_attempted = false;
    int result = ERR_IO;
    if (!configure()) goto done;
    /* The speaker stays muted throughout capture. */
    if (!hal_i2c_sys_update_reg(0x43,5,2,0) || !hal_i2c_sys_write_reg(0x10,25,0x24)) goto done;
    s_allocation = kmalloc_internal(RING_BYTES+63);
    if (!s_allocation) { result = ERR_NOMEM; goto done; }
    s_pcm = (int16_t *)(((uintptr_t)s_allocation+63)&~(uintptr_t)63);
    memset(s_pcm,0,RING_BYTES);
    s_codec = true; /* partial codec setup must be powered down on every failure */
    if (!mic_codec_start()) goto done;
    for (unsigned i=0;i<BLOCKS;i++) s_desc[i].d = (dma_descriptor_t){
        .dw0={.size=MIC_BLOCK_BYTES,.length=0,.owner=1},
        .buffer=s_pcm+i*MIC_BLOCK_FRAMES*MIC_SLOTS,.next=&s_desc[(i+1)%BLOCKS].d};
    esp_cache_msync(s_pcm,RING_BYTES,ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    esp_cache_msync(s_desc,sizeof s_desc,ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    __asm__ volatile("fence iorw,iorw" ::: "memory");
    ahb_dma_ll_rx_clear_interrupt_status(&AHB_DMA,CH,GDMA_LL_RX_EVENT_MASK);
    i2s_ll_rx_reset_fifo(&I2S0);
    ahb_dma_ll_rx_set_desc_addr(&AHB_DMA,CH,(uint32_t)s_desc);
    ahb_dma_ll_rx_start(&AHB_DMA,CH); i2s_ll_rx_start(&I2S0);
    kprintf("mic: capturing %u seconds, 48kHz four raw slots; no saving or transmission\n",seconds);
    unsigned next=0;
    uint64_t end=hal_time_now_us()+(uint64_t)seconds*1000000u, progress=hal_time_now_us();
    while (hal_time_now_us()<end) {
        /* Descriptor empty/error means an overrun or corrupt chain. Stop, do not silently drop data. */
        uint32_t flags=ahb_dma_ll_rx_get_interrupt_status(&AHB_DMA,CH,true);
        if (flags & (GDMA_LL_EVENT_RX_DESC_ERROR | GDMA_LL_EVENT_RX_DESC_EMPTY | GDMA_LL_EVENT_RX_ERR_EOF)) {
            s_errors++; goto done;
        }
        for (unsigned n=0;n<BLOCKS;n++) {
            esp_cache_msync(&s_desc[next],64,ESP_CACHE_MSYNC_FLAG_DIR_M2C);
            volatile dma_descriptor_t *d=&s_desc[next].d;
            if (d->dw0.owner) break;
            size_t bytes=d->dw0.length;
            if (d->dw0.err_eof || bytes != MIC_BLOCK_BYTES) { s_errors++; goto done; }
            int16_t *pcm=s_pcm+next*MIC_BLOCK_FRAMES*MIC_SLOTS;
            esp_cache_msync(pcm,MIC_BLOCK_BYTES,ESP_CACHE_MSYNC_FLAG_DIR_M2C);
            if (!mic_levels_add(&s_levels,pcm,bytes)) { s_errors++; goto done; }
            d->dw0.length=0; d->dw0.suc_eof=0; d->dw0.owner=1;
            __asm__ volatile("fence iorw,iorw" ::: "memory");
            esp_cache_msync(&s_desc[next],64,ESP_CACHE_MSYNC_FLAG_DIR_C2M);
            s_blocks++; next=(next+1)%BLOCKS; progress=hal_time_now_us();
        }
        if (hal_time_now_us()-progress>500000) { result=ERR_TIMEOUT; s_errors++; goto done; }
        task_sleep(2);
    }
    result=s_blocks ? ERR_OK : ERR_TIMEOUT;
done:
    stop(); summary();
    return !s_powerdown_ok && result==ERR_OK ? ERR_IO : result;
}
int microphone_command(int argc, char **argv)
{
    unsigned seconds=3;
    if (argc<2 || argc>3 || strcmp(argv[1],"test")) {
        kputs("usage: mic test [1..10 seconds] (exclusive input; no recording file)\n"); return 1;
    }
    if (argc==3) {
        seconds=0;
        for (const char *p=argv[2];*p;p++) {
            if (*p<'0' || *p>'9' || seconds>10) return 1;
            seconds=seconds*10+(unsigned)(*p-'0');
        }
        if (seconds<1 || seconds>10) return 1;
    }
    if (!task_has_perm(APP_PERM_INPUT)) return 1;
    if (!mutex_trylock(&s_lock)) { kputs("mic: busy\n"); return 1; }
    int rc=capture(seconds);
    mutex_unlock(&s_lock);
    if (rc!=ERR_OK) kprintf("mic: capture failed (%d)\n",rc);
    return rc==ERR_OK ? 0 : 1;
}
#endif
