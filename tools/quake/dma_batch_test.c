/* Run the production DMA driver against delayed descriptor-chain hardware.
 * Completing only from its polling clock catches reuse of live source banks
 * and descriptor arrays; compare every framebuffer pixel and surrounding RAM. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../drivers/lcd/dma_copy_tab5.c"
dma2d_dev_t mock_hw;
static bool active;
static bool fail_internal;
static unsigned internal_allocations;
static unsigned submitted,links;
static dma2d_descriptor_t *live_tx[8],*live_rx[8],saved_tx[8],saved_rx[8];
static uint8_t saved_source[8][1536];
void mock_dma_reset(void){assert(!active);}
void mock_dma_start(void){
 assert(!active);active=true;submitted++;links=0;
 dma2d_descriptor_t *t=(void *)mock_hw.tx_head,*r=(void *)mock_hw.rx_head;
 while(t&&r){
  assert(links<8&&t->owner==1&&r->owner==1);
  assert(t->vb_size==r->vb_size&&t->hb_length==t->vb_size);
  assert(t->vb_size>0&&t->vb_size<=sizeof saved_source[0]&&!(t->vb_size&3));
  assert(t->suc_eof==r->suc_eof&&t->suc_eof==(t->next==NULL));
  assert((t->next==NULL)==(r->next==NULL));
  assert(!((uintptr_t)t&127u)&&!((uintptr_t)r&127u));
  live_tx[links]=t;live_rx[links]=r;saved_tx[links]=*t;saved_rx[links]=*r;
  memcpy(saved_source[links],t->buffer,t->vb_size);links++;t=t->next;r=r->next;
 }
 assert(!t&&!r&&links>=QV_REPEAT&&links%QV_REPEAT==0);
}
uint64_t hal_time_now_us(void){
 static uint64_t ticks;
 if(active){
  for(unsigned i=0;i<links;i++){
   assert(!memcmp(live_tx[i],&saved_tx[i],sizeof saved_tx[i]));
   assert(!memcmp(live_rx[i],&saved_rx[i],sizeof saved_rx[i]));
   assert(!memcmp(live_tx[i]->buffer,saved_source[i],live_tx[i]->vb_size));
   memcpy(live_rx[i]->buffer,live_tx[i]->buffer,live_tx[i]->vb_size);
  }
  active=false;mock_hw.in_channel[0].in_int_raw.val=DMA2D_LL_EVENT_RX_SUC_EOF;
 }
 return ticks+=10;
}
uint32_t arch_irq_disable(void){return 0;}
void arch_irq_restore(uint32_t flags){(void)flags;}
void *kmalloc_psram(size_t n){return malloc(n);}
void *kmalloc_internal(size_t n){if(fail_internal)return NULL;internal_allocations++;return malloc(n);}
void kputs(const char *s){(void)s;}
void kprintf(const char *fmt,...){assert(!strstr(fmt,"stopped, status"));}
int esp_cache_msync(void *p,size_t n,int flags){(void)flags;assert(!((uintptr_t)p&127u)&&!(n&127u));return 0;}
static uint16_t actual[1280*720+128] __attribute__((aligned(128))),expected[1280*720+128],rgb[QV_PIXELS];
static uint8_t pixels[QV_PIXELS] __attribute__((aligned(16))),previous[QV_PIXELS] __attribute__((aligned(16))),unchanged[QV_PIXELS];
int main(void){
 uint16_t palette[256];for(unsigned i=0;i<256;i++)palette[i]=(uint16_t)(i*239);
 memset(actual,0x5a,sizeof actual);memcpy(expected,actual,sizeof actual);
 for(unsigned i=0;i<QV_PIXELS;i++)pixels[i]=(uint8_t)(i*11);
#if M5OS_QUAKE_HIGHRES
 fail_internal=true;assert(!tab5_dma_copy_begin(7,actual+64));assert(!owner&&!mock_hw.clock&&!active);
 fail_internal=false;
#endif
 assert(tab5_dma_copy_begin(7,actual+64));assert(!active&&!tab5_dma_copy_begin(9,actual+64));
 for(unsigned f=0;f<32;f++){
  bool force=f==0||f==20;unsigned changed=0;
  if(f>1&&f<20){
   changed=1+f%9;
   const unsigned cols[9]={1,31,63,65,127,129,257,QV_WIDTH-2,QV_WIDTH-1};
   for(unsigned i=0;i<changed;i++){
    unsigned y=(f*17+i*29)%(QV_HEIGHT-1);pixels[y*QV_WIDTH+cols[i]]^=(uint8_t)(f+1);
    pixels[(QV_HEIGHT-1)*QV_WIDTH+cols[i]]^=(uint8_t)(f+1);
   }
  }else if(f==20)palette[17]^=0xffff;
  else if(f>20){changed=QV_WIDTH;for(unsigned i=0;i<QV_PIXELS;i++)pixels[i]=(uint8_t)(i+f);}
  memcpy(unchanged,pixels,sizeof pixels);unsigned before=submitted;
  assert(tab5_dma_copy_present(pixels,previous,palette,force));assert(!active);
  unsigned columns=force?QV_WIDTH:changed;
#if M5OS_QUAKE_HIGHRES
  assert(submitted-before==(columns+DMA_BATCH_COLS-1)/DMA_BATCH_COLS);
#else
  if(submitted-before!=columns)fprintf(stderr,"frame %u got %u expected %u\n",f,submitted-before,columns);
  assert(submitted-before==columns);
#endif
  for(unsigned i=0;i<QV_PIXELS;i++)rgb[i]=palette[pixels[i]];
  int a,b;assert(tab5_bitmap_resize(expected+64,QV_X,QV_Y,QV_WIDTH,QV_HEIGHT,rgb,QV_OUT_WIDTH,QV_OUT_HEIGHT,&a,&b));
  assert(!memcmp(actual,expected,sizeof actual));assert(!memcmp(previous,pixels,sizeof pixels));assert(!memcmp(unchanged,pixels,sizeof pixels));
 }
 tab5_dma_copy_end(9);assert(owner==7);tab5_dma_copy_end(7);assert(!active&&!mock_hw.clock&&!owner);
 assert(tab5_dma_copy_begin(9,actual+64));tab5_dma_copy_end(9);
#if M5OS_QUAKE_HIGHRES
 assert(internal_allocations==1);
#endif
 puts("DMA production driver: delayed chains/bank lifetime, sparse partial batches, >255 rows, palette, independent framebuffer and guards PASS");
}
