/* Experimental exact RGB565 column DMA. Register setup follows ESP-IDF 5.4.4
 * DMA2D LL / dma2d.c (Espressif Apache-2.0); no FreeRTOS driver is linked. */
#include <m5os/board_config.h>
#if M5OS_BOARD_TAB5 && M5OS_QUAKE
#include "dma_copy_tab5.h"
#include "dma_column.h"
#include "bitmap_resize.h"
#include "quake_hires.h"
#include <m5os/arch.h>
#include <m5os/kmalloc.h>
#include <m5os/hal_time.h>
#include <m5os/kprintf.h>
#include "hal/dma2d_ll.h"
#include "esp_cache.h"
#include <string.h>
static dma2d_dev_t *const hw=DMA2D_LL_GET_HW(0);
static uint32_t owner;
static bool claimed,pending,disabled;
static uint16_t *fb,*test;
static void *test_raw;
/* Kernel-owned SRAM buffers survive forced app termination. Never use a task
 * stack or owner-reclaimed allocation for an outstanding transfer. */
#if M5OS_QUAKE_HIGHRES
#define DMA_BATCH_COLS 2
#define DMA_DESC_MAX (DMA_BATCH_COLS*QV_REPEAT)
/* Each 600-pixel expanded column is padded to cache-line boundaries. */
static uint16_t (*source)[DMA_BATCH_COLS][640];
static void *source_raw;
#else
#define DMA_DESC_MAX 3
static uint16_t source[2][768] __attribute__((aligned(128)));
#endif
static union {dma2d_descriptor_t d;uint8_t line[128];} tx[DMA_DESC_MAX] __attribute__((aligned(128))),rx[DMA_DESC_MAX] __attribute__((aligned(128)));
static bool clocks(bool enable){
 uint32_t irq=arch_irq_disable();int __DECLARE_RCC_ATOMIC_ENV=0;
 if(enable&&dma2d_ll_is_bus_clock_enabled(0)){arch_irq_restore(irq);return false;}
 if(enable){dma2d_ll_enable_bus_clock(0,true);dma2d_ll_reset_register(0);}
 else if(claimed){dma2d_ll_reset_register(0);dma2d_ll_enable_bus_clock(0,false);}
 claimed=enable;arch_irq_restore(irq);return true;
}
static bool wait_done(void){
 if(!pending)return !disabled;
 uint64_t limit=hal_time_now_us()+100000;
 for(;;){
  uint32_t status=hw->in_channel[0].in_int_raw.val;
  /* A completed transfer stays successful even if a higher-priority task
   * delayed this poll past the deadline. Hardware errors still win. */
  bool error=status&(DMA2D_LL_EVENT_RX_DESC_ERROR|DMA2D_LL_EVENT_RX_ERR_EOF);
  if(!error&&(status&DMA2D_LL_EVENT_RX_SUC_EOF)){pending=false;return true;}
  if(error||hal_time_now_us()>limit){
   clocks(false);pending=false;disabled=true;kprintf("dma-copy: stopped, status %08x; disabled until reset\n",status);return false;
  }
 }
}
static void sync_buffer(void *p,size_t n,int flags){esp_cache_msync(p,n,flags);}
static bool start_chain(unsigned descriptors){
 dma2d_ll_tx_reset_channel(hw,0);dma2d_ll_rx_reset_channel(hw,0);
 sync_buffer(tx,descriptors*sizeof tx[0],ESP_CACHE_MSYNC_FLAG_DIR_C2M);sync_buffer(rx,descriptors*sizeof rx[0],ESP_CACHE_MSYNC_FLAG_DIR_C2M);
 dma2d_ll_tx_enable_interrupt(hw,0,UINT32_MAX,false);dma2d_ll_rx_enable_interrupt(hw,0,UINT32_MAX,false);
 dma2d_ll_tx_clear_interrupt_status(hw,0,UINT32_MAX);dma2d_ll_rx_clear_interrupt_status(hw,0,UINT32_MAX);
 dma2d_ll_tx_connect_to_periph(hw,0,DMA2D_TRIG_PERIPH_M2M,4);dma2d_ll_rx_connect_to_periph(hw,0,DMA2D_TRIG_PERIPH_M2M,3);
 dma2d_ll_tx_enable_dscr_port(hw,0,false);dma2d_ll_rx_enable_dscr_port(hw,0,false);
 dma2d_ll_tx_enable_owner_check(hw,0,false);dma2d_ll_rx_enable_owner_check(hw,0,false);
 dma2d_ll_tx_enable_eof_mode(hw,0,true);dma2d_ll_tx_enable_auto_write_back(hw,0,false);dma2d_ll_rx_set_auto_return_owner(hw,0,0);
 dma2d_ll_tx_enable_reorder(hw,0,false);dma2d_ll_rx_enable_reorder(hw,0,false);
 dma2d_ll_tx_enable_page_bound_wrap(hw,0,true);dma2d_ll_rx_enable_page_bound_wrap(hw,0,true);
 dma2d_ll_tx_enable_descriptor_burst(hw,0,true);dma2d_ll_rx_enable_descriptor_burst(hw,0,true);
 dma2d_ll_tx_set_data_burst_length(hw,0,DMA2D_DATA_BURST_LENGTH_128);dma2d_ll_rx_set_data_burst_length(hw,0,DMA2D_DATA_BURST_LENGTH_128);
 dma2d_ll_tx_set_macro_block_size(hw,0,DMA2D_MACRO_BLOCK_SIZE_NONE);dma2d_ll_rx_set_macro_block_size(hw,0,DMA2D_MACRO_BLOCK_SIZE_NONE);
 dma2d_ll_tx_configure_color_space_conv(hw,0,DMA2D_CSC_TX_NONE);dma2d_ll_rx_configure_color_space_conv(hw,0,DMA2D_CSC_RX_NONE);
 dma2d_ll_tx_set_desc_addr(hw,0,(uintptr_t)&tx[0].d);dma2d_ll_rx_set_desc_addr(hw,0,(uintptr_t)&rx[0].d);
 pending=true;dma2d_ll_rx_start(hw,0);dma2d_ll_tx_start(hw,0);return true;
}
static void set_descriptor(unsigned n,unsigned total,uint16_t *src,uint16_t *dst,unsigned bytes){
 tx[n].d=(dma2d_descriptor_t){.vb_size=bytes,.hb_length=bytes,.owner=1,.suc_eof=n==total-1,.buffer=src,.next=n+1<total?&tx[n+1].d:NULL};
 rx[n].d=(dma2d_descriptor_t){.vb_size=bytes,.owner=1,.suc_eof=n==total-1,.buffer=dst,.next=n+1<total?&rx[n+1].d:NULL};
}
static bool transfer(uint16_t *src,uint16_t *dst,unsigned bytes){
 if(!wait_done())return false;
 memset(tx,0,sizeof tx);memset(rx,0,sizeof rx);
 for(unsigned i=0;i<QV_REPEAT;i++)set_descriptor(i,QV_REPEAT,src,dst+i*720,bytes);
 return start_chain(QV_REPEAT);
}
#if M5OS_QUAKE_HIGHRES
typedef struct {unsigned col,start,end;} column_span;
static bool transfer_batch(unsigned bank,const column_span *spans,unsigned count){
 /* The other bank can be active while this bank is filled/flushed. Shared
  * descriptors must not be modified until the preceding chain completes. */
 sync_buffer(source[bank],count*sizeof source[bank][0],ESP_CACHE_MSYNC_FLAG_DIR_C2M);
 if(!wait_done())return false;
 memset(tx,0,sizeof tx);memset(rx,0,sizeof rx);
 unsigned total=count*QV_REPEAT;
 for(unsigned c=0;c<count;c++)for(unsigned r=0;r<QV_REPEAT;r++){
  unsigned start=spans[c].start;
  uint16_t *dst=fb+(1280-QV_X-QV_REPEAT-spans[c].col*QV_REPEAT+r)*720+QV_Y+start;
  set_descriptor(c*QV_REPEAT+r,total,source[bank][c]+start,dst,(spans[c].end-start)*2);
 }
 return start_chain(total);
}
#endif
bool tab5_dma_copy_begin(uint32_t who,uint16_t *framebuffer){
 if(!who||owner||disabled||!framebuffer||((uintptr_t)framebuffer&127u)){
  kprintf("dma-copy: begin rejected who %u owner %u disabled %u fb alignment %u\n",who,owner,(unsigned)disabled,(unsigned)((uintptr_t)framebuffer&127u));return false;
 }
 if(!clocks(true)){kputs("dma-copy: existing DMA2D clock owner; CPU retained\n");return false;}
 owner=who;fb=framebuffer;dma2d_ll_hw_enable(hw,true);
#if M5OS_QUAKE_HIGHRES
 /* Linked internal DRAM is tight. Allocate once from the kernel's internal
  * heap, with no app owner, so buffers remain alive even after a forced exit. */
 if(!source_raw)source_raw=kmalloc_internal(2*sizeof *source+127);
 if(!source_raw){kputs("dma-copy: no internal memory for source banks\n");tab5_dma_copy_end(who);return false;}
 source=(void *)(((uintptr_t)source_raw+127)&~(uintptr_t)127);
#endif
 if(!test_raw)test_raw=kmalloc_psram(4608+127);
 if(!test_raw){kputs("dma-copy: no PSRAM for self-test\n");tab5_dma_copy_end(who);return false;}
 test=(uint16_t *)(((uintptr_t)test_raw+127)&~(uintptr_t)127);
#if M5OS_QUAKE_HIGHRES
 uint16_t *probe=source[0][0];unsigned probe_pixels=600,probe_capacity=640;
#else
 uint16_t *probe=source[0];unsigned probe_pixels=720,probe_capacity=768;
#endif
 for(unsigned i=0;i<probe_capacity;i++)probe[i]=(uint16_t)(i*17+3);
 for(unsigned i=0;i<2304;i++)test[i]=0x5a5a;
 sync_buffer(probe,probe_capacity*2,ESP_CACHE_MSYNC_FLAG_DIR_C2M);
 sync_buffer(test,4608,ESP_CACHE_MSYNC_FLAG_DIR_C2M|ESP_CACHE_MSYNC_FLAG_INVALIDATE);
 bool ok=transfer(probe,test+64,probe_pixels*2)&&wait_done();
 if(ok){
  sync_buffer(test,4608,ESP_CACHE_MSYNC_FLAG_DIR_M2C);
  for(unsigned i=0;i<2304;i++)if(test[i]!=(i>=64&&(i-64)/720<QV_REPEAT&&(i-64)%720<probe_pixels?probe[(i-64)%720]:0x5a5a))ok=false;
 }
 if(!ok){disabled=true;kputs("dma-copy: offscreen self-test failed; CPU retained\n");tab5_dma_copy_end(who);return false;}
 kputs("dma-copy: offscreen linear/triplet/guards PASS\n");return true;
}
bool tab5_dma_copy_present(const uint8_t *pixels,uint8_t *previous,const uint16_t palette[256],bool force){
 if(!owner||disabled||!wait_done())return false;
 unsigned left,right;
#if M5OS_QUAKE_HIGHRES
 uint16_t first[QV_WIDTH],last[QV_WIDTH];
 if(!tab5_hires_damage(pixels,previous,force,first,last,&left,&right))return true;
#else
 uint8_t first[320],last[320];
 if(!tab5_indexed_damage(pixels,previous,force,first,last,&left,&right))return true;
#endif
 /* Viewport boundaries are cache-line aligned. Flush/invalidate once before
  * DMA; CPU never writes inside this region until the completed frame returns. */
 sync_buffer(fb+(1280-QV_X-QV_OUT_WIDTH)*720,QV_OUT_WIDTH*720*2,ESP_CACHE_MSYNC_FLAG_DIR_C2M|ESP_CACHE_MSYNC_FLAG_INVALIDATE);
 unsigned buffer=0;
#if M5OS_QUAKE_HIGHRES
 uint8_t tile[TAB5_HIRES_TILE_COLS][300];
 column_span spans[DMA_BATCH_COLS];unsigned queued=0;
 for(unsigned base=left;base<=right;base+=TAB5_HIRES_TILE_COLS){
  unsigned count=right-base+1;if(count>TAB5_HIRES_TILE_COLS)count=TAB5_HIRES_TILE_COLS;
  bool dirty=false;for(unsigned i=0;i<count;i++)if(first[base+i]<QV_HEIGHT)dirty=true;
  if(!dirty)continue;
  tab5_hires_stage(tile,pixels,base,count);
  for(unsigned i=0;i<count;i++){
   unsigned col=base+i;if(first[col]>=QV_HEIGHT)continue;
   uint16_t *src=source[buffer][queued];
   tab5_hires_expand(src,tile[i],palette,first[col],last[col]);
   unsigned start=first[col]*2,end=(last[col]+1)*2;
   spans[queued++]=(column_span){col,start,end};
   if(queued==DMA_BATCH_COLS){if(!transfer_batch(buffer,spans,queued))return false;buffer^=1;queued=0;}
  }
 }
 if(queued&&!transfer_batch(buffer,spans,queued))return false;
#else
 for(unsigned col=left;col<=right;col++){
  if(first[col]>=QV_HEIGHT)continue;
  uint16_t *src=source[buffer];buffer^=1;tab5_dma_column(src,pixels,palette,col);
  sync_buffer(src,sizeof source[0],ESP_CACHE_MSYNC_FLAG_DIR_C2M);
  if(!wait_done())return false;
  unsigned start=(((unsigned)first[col]*18+4)/5)&~1u;
  unsigned end=((((unsigned)last[col]+1)*18+4)/5+1)&~1u;
  if(end>720)end=720;
  if(!transfer(src+start,fb+(1280-QV_X-QV_REPEAT-col*QV_REPEAT)*720+QV_Y+start,(end-start)*2))return false;
 }
#endif
 return wait_done();
}
void tab5_dma_copy_end(uint32_t who){
 if(!who||who!=owner)return;
 wait_done();if(claimed)clocks(false);pending=false;owner=0;fb=NULL;
}
#endif
