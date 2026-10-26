/* Local experimental P4 SRM backend. Register protocol follows ESP-IDF 5.4.4
 * ppa_srm.c/dma2d.c (Espressif, Apache-2.0); only LL headers are linked here.
 * No FreeRTOS queues/semaphores/interrupt callbacks are used. */
#include <m5os/board_config.h>
#if M5OS_BOARD_TAB5 && M5OS_QUAKE
#include "ppa_tab5.h"
#include "ppa_stage.h"
#include <m5os/arch.h>
#include <m5os/kmalloc.h>
#include <m5os/hal_time.h>
#include <m5os/kprintf.h>
#include "hal/dma2d_ll.h"
#include "hal/dma2d_types.h"
#include "hal/ppa_ll.h"
#include "soc/dma2d_channel.h"
#include "esp_cache.h"
#include <string.h>
static uint32_t owner;
static bool pending,failed,hardware_owned,disabled;
static void *stage_raw,*test_raw;
static uint16_t *stage,*fb;
static dma2d_dev_t *const dma=DMA2D_LL_GET_HW(0);
static ppa_dev_t *const ppa=PPA_LL_GET_HW;
/* Each descriptor occupies its own complete largest cache line. */
static union {dma2d_descriptor_t desc;uint8_t bytes[128];} descriptors[2] __attribute__((aligned(128)));
static bool clocks(bool enable){
 uint32_t irq=arch_irq_disable();int __DECLARE_RCC_ATOMIC_ENV=0;
 if(enable&&(dma2d_ll_is_bus_clock_enabled(0)||HP_SYS_CLKRST.soc_clk_ctrl1.reg_ppa_sys_clk_en)){arch_irq_restore(irq);return false;}
 if(!enable&&!hardware_owned){arch_irq_restore(irq);return true;}
 if(enable){dma2d_ll_enable_bus_clock(0,true);dma2d_ll_reset_register(0);ppa_ll_enable_bus_clock(true);ppa_ll_reset_register();}
 else{ppa_ll_reset_register();dma2d_ll_reset_register(0);ppa_ll_enable_bus_clock(false);dma2d_ll_enable_bus_clock(0,false);}
 hardware_owned=enable;arch_irq_restore(irq);return true;
}
static bool wait_done(void){
 if(!pending)return !failed;
 uint64_t deadline=hal_time_now_us()+100000;
 for(;;){
  uint32_t status=dma->in_channel[0].in_int_raw.val & DMA2D_LL_RX_EVENT_MASK;
  if((status&(DMA2D_LL_EVENT_RX_DESC_ERROR|DMA2D_LL_EVENT_RX_ERR_EOF))||(!(status&DMA2D_LL_EVENT_RX_SUC_EOF)&&hal_time_now_us()>deadline)){
   /* We acquired both previously clock-gated modules exclusively. Reset them
    * before returning to CPU presentation. Buffers remain quarantined. */
   clocks(false);pending=false;failed=true;disabled=true;kprintf("ppa: stopped, status %08x\n",status);return false;
  }
  if(status&DMA2D_LL_EVENT_RX_SUC_EOF){pending=false;return true;}
 }
}
static bool transfer(uint16_t *source,unsigned sw,unsigned sh,uint16_t *out,unsigned ow,unsigned oh,unsigned ox,unsigned oy){
 if(!wait_done())return false;
 dma2d_ll_tx_reset_channel(dma,0);dma2d_ll_rx_reset_channel(dma,0);ppa_ll_srm_reset(ppa);
 dma2d_descriptor_t *tx=&descriptors[0].desc,*rx=&descriptors[1].desc;
 memset(descriptors,0,sizeof descriptors);
 *tx=(dma2d_descriptor_t){.vb_size=sh,.hb_length=sw,.dma2d_en=1,.suc_eof=1,.owner=1,.va_size=sh,.ha_length=sw,.pbyte=DMA2D_DESCRIPTOR_PBYTE_2B0_PER_PIXEL,.buffer=source};
 *rx=(dma2d_descriptor_t){.vb_size=2,.hb_length=2,.dma2d_en=1,.suc_eof=1,.owner=1,.va_size=oh,.ha_length=ow,.x=ox,.y=oy,.buffer=out};
 /* Source and output regions are aligned and sized to 128 bytes in all callers. */
 esp_cache_msync(source,sw*sh*2,ESP_CACHE_MSYNC_FLAG_DIR_C2M);
 esp_cache_msync(out,ow*oh*2,ESP_CACHE_MSYNC_FLAG_DIR_C2M|ESP_CACHE_MSYNC_FLAG_INVALIDATE);
 esp_cache_msync(descriptors,sizeof descriptors,ESP_CACHE_MSYNC_FLAG_DIR_C2M);
 dma2d_ll_tx_enable_interrupt(dma,0,UINT32_MAX,false);dma2d_ll_rx_enable_interrupt(dma,0,UINT32_MAX,false);
 dma2d_ll_tx_clear_interrupt_status(dma,0,UINT32_MAX);dma2d_ll_rx_clear_interrupt_status(dma,0,UINT32_MAX);
 dma2d_ll_tx_connect_to_periph(dma,0,DMA2D_TRIG_PERIPH_PPA_SRM,SOC_DMA2D_TRIG_PERIPH_PPA_SRM_TX);
 dma2d_ll_rx_connect_to_periph(dma,0,DMA2D_TRIG_PERIPH_PPA_SRM,SOC_DMA2D_TRIG_PERIPH_PPA_SRM_RX);
 dma2d_ll_tx_enable_dscr_port(dma,0,true);dma2d_ll_rx_enable_dscr_port(dma,0,true);
 dma2d_ll_tx_enable_owner_check(dma,0,false);dma2d_ll_rx_enable_owner_check(dma,0,false);
 dma2d_ll_tx_enable_eof_mode(dma,0,true);dma2d_ll_tx_enable_auto_write_back(dma,0,false);
 dma2d_ll_rx_set_auto_return_owner(dma,0,0);
 dma2d_ll_tx_enable_reorder(dma,0,false);dma2d_ll_rx_enable_reorder(dma,0,false);
 dma2d_ll_tx_enable_page_bound_wrap(dma,0,true);dma2d_ll_rx_enable_page_bound_wrap(dma,0,true);
 dma2d_ll_tx_enable_descriptor_burst(dma,0,true);dma2d_ll_rx_enable_descriptor_burst(dma,0,true);
 dma2d_ll_tx_set_data_burst_length(dma,0,DMA2D_DATA_BURST_LENGTH_128);dma2d_ll_rx_set_data_burst_length(dma,0,DMA2D_DATA_BURST_LENGTH_128);
 dma2d_ll_tx_set_macro_block_size(dma,0,DMA2D_MACRO_BLOCK_SIZE_NONE);dma2d_ll_rx_set_macro_block_size(dma,0,DMA2D_MACRO_BLOCK_SIZE_NONE);
 dma2d_ll_tx_configure_color_space_conv(dma,0,DMA2D_CSC_TX_NONE);dma2d_ll_rx_configure_color_space_conv(dma,0,DMA2D_CSC_RX_NONE);
 ppa_ll_srm_set_mb_size(ppa,PPA_LL_SRM_MB_SIZE_16_16);
 uint32_t bh,bv;ppa_ll_srm_get_dma_dscr_port_mode_block_size(ppa,PPA_SRM_COLOR_MODE_RGB565,PPA_LL_SRM_MB_SIZE_16_16,&bh,&bv);
 dma2d_ll_tx_set_dscr_port_block_size(dma,0,bh,bv);
 dma2d_ll_tx_set_desc_addr(dma,0,(uintptr_t)tx);dma2d_ll_rx_set_desc_addr(dma,0,(uintptr_t)rx);
 dma2d_ll_tx_start(dma,0);dma2d_ll_rx_start(dma,0);
 ppa_ll_srm_set_rx_color_mode(ppa,PPA_SRM_COLOR_MODE_RGB565);ppa_ll_srm_set_tx_color_mode(ppa,PPA_SRM_COLOR_MODE_RGB565);
 ppa_ll_srm_enable_rx_byte_swap(ppa,false);ppa_ll_srm_enable_rx_rgb_swap(ppa,false);
 ppa_ll_srm_configure_rx_alpha(ppa,PPA_ALPHA_NO_CHANGE,0);
 ppa_ll_srm_set_rotation_angle(ppa,PPA_SRM_ROTATION_ANGLE_90);
 ppa_ll_srm_set_scaling_x(ppa,3,0);ppa_ll_srm_set_scaling_y(ppa,1,0);
 ppa_ll_srm_enable_mirror_x(ppa,false);ppa_ll_srm_enable_mirror_y(ppa,false);
 /* DIG-734 workaround, identical condition to the SDK for RGB565/16x16 MB. */
 unsigned width=sw*3,left=width%64;if(!left)left=64;unsigned height=sh%16;if(!height)height=16;
 ppa_ll_srm_bypass_mb_order(ppa,(width>64||sh>16)&&left*height*16<1536);
 pending=true;ppa_ll_srm_start(ppa);return true;
}
bool tab5_ppa_begin(uint32_t who,uint16_t *framebuffer){
 if(!who||owner||disabled||!framebuffer||((uintptr_t)framebuffer&127u))return false;
 /* Existing SDK users are never reset or commandeered. */
 if(dma2d_ll_is_bus_clock_enabled(0)||HP_SYS_CLKRST.soc_clk_ctrl1.reg_ppa_sys_clk_en){kputs("ppa: hardware busy; classic renderer retained\n");return false;}
 /* Kernel-owned reusable buffers are not reclaimed by forced app teardown.
  * On a DMA fault they remain quarantined, with the backend disabled until
  * reboot, so late AXI transactions cannot touch recycled app allocations. */
 uint32_t alloc_irq=arch_irq_disable();
 if(!stage_raw)stage_raw=kmalloc_psram(320*720*2+127);
 if(!test_raw)test_raw=kmalloc_psram(4096+127);
 arch_irq_restore(alloc_irq);
 if(!stage_raw||!test_raw)return false;
 stage=(uint16_t *)(((uintptr_t)stage_raw+127)&~(uintptr_t)127);fb=framebuffer;owner=who;failed=false;
 if(!clocks(true)){owner=0;stage=NULL;fb=NULL;return false;}
 dma2d_ll_hw_enable(dma,true);
 /* Offscreen quadrants + guard border validate orientation and bounded writes. */
 void *raw=test_raw;
 uint16_t *out=(uint16_t *)(((uintptr_t)raw+127)&~(uintptr_t)127);
 for(unsigned y=0;y<16;y++)for(unsigned x=0;x<16;x++)stage[y*16+x]=y<8?(x<8?0xf800:0x001f):(x<8?0x07e0:0xffe0);
 for(unsigned i=0;i<2048;i++)out[i]=0x5a5a;
 bool ok=transfer(stage,16,16,out,32,64,8,8)&&wait_done();
 if(ok){
  esp_cache_msync(out,4096,ESP_CACHE_MSYNC_FLAG_DIR_M2C);
  ok=out[(8+6)*32+8+4]==0x001f&&out[(8+6)*32+8+12]==0xffe0&&out[(8+42)*32+8+4]==0xf800&&out[(8+42)*32+8+12]==0x07e0;
  for(unsigned y=0;y<64;y++)for(unsigned x=0;x<32;x++)if((x<8||x>=24||y<8||y>=56)&&out[y*32+x]!=0x5a5a)ok=false;
 }
 if(!ok){disabled=true;kputs("ppa: offscreen self-test failed; classic renderer retained\n");tab5_ppa_end(who);return false;}
 kputs("ppa: offscreen rotation/bounds PASS; smooth mode ready\n");return true;
}
bool tab5_ppa_present(const uint8_t *pixels,const uint16_t palette[256]){
 if(!owner||failed||!wait_done())return false;
 /* Preserve the exact vertical 200->720 sample positions in software. Only
  * horizontal 3x scaling is bilinear; SRM rotates into the portrait framebuffer. */
 tab5_ppa_stage(stage,pixels,palette);
 return transfer(stage,320,720,fb,720,1280,0,160);
}
void tab5_ppa_end(uint32_t who){
 if(!who||owner!=who)return;
 wait_done();if(hardware_owned)clocks(false);
 stage=NULL;fb=NULL;owner=0;pending=false;
}
#endif
