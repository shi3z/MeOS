#include <m5os/keyboard_raw.h>
#include <m5os/arch.h>
#include <string.h>
static const void *owner;
static uint32_t generation;
static keyboard_raw_state state;
static uint8_t matrix[70];
/* Physical base matrix from M5Stack's documented Tab5 5x14 layout. Native mode
 * treats Aa as Shift and Sym as the symbol-layer modifier; text mode is untouched. */
static const uint8_t base[70]={
 0x29,0x1e,0x1f,0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x27,0x2d,0x2e,0x4c,
 0x35,0x1e,0x1f,0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x27,0x2f,0x30,0x31,
 0x2b,0x14,0x1a,0x08,0x15,0x17,0x1c,0x18,0x0c,0x12,0x13,0x33,0x34,0x2a,
 0,0,0x04,0x16,0x07,0x09,0x0a,0x0b,0x0d,0x0e,0x0f,0x52,0x2d,0x28,
 0,0,0x1d,0x1b,0x06,0x19,0x05,0x11,0x10,0x37,0x50,0x51,0x4f,0x2c};
static void bit(uint8_t keys[32],uint8_t k){if(k)keys[k/8]|=(uint8_t)(1u<<(k%8));}
static bool push(uint8_t usage,bool down){
 if(state.count>=KEYBOARD_RAW_EVENTS){memset(&state,0,sizeof state);memset(matrix,0,sizeof matrix);return false;}
 state.events[state.count++]=(keyboard_raw_event){usage,down};return true;
}
static void publish(const uint8_t keys[32],uint8_t mods){
 for(unsigned m=0;m<8;m++)if((mods&~state.mods)&(1u<<m))if(!push((uint8_t)(0xe0+m),true))return;
 for(unsigned k=4;k<224;k++){
  bool old=(state.keys[k/8]&(1u<<(k%8)))!=0,now=(keys[k/8]&(1u<<(k%8)))!=0;
  if(old!=now&&!push((uint8_t)k,now))return;
 }
 for(unsigned m=0;m<8;m++)if((state.mods&~mods)&(1u<<m))if(!push((uint8_t)(0xe0+m),false))return;
 memcpy(state.keys,keys,32);state.mods=mods;state.ready=true;
}
bool keyboard_raw_claim(const void *who){if(!who)return false;uint32_t f=arch_irq_disable();bool ok=!owner||owner==who;if(ok&&!owner){owner=who;generation++;memset(&state,0,sizeof state);memset(matrix,0,sizeof matrix);}arch_irq_restore(f);return ok;}
void keyboard_raw_release(const void *who){uint32_t f=arch_irq_disable();if(owner&&owner==who){owner=NULL;generation++;memset(&state,0,sizeof state);memset(matrix,0,sizeof matrix);}arch_irq_restore(f);}
bool keyboard_raw_active(void){uint32_t f=arch_irq_disable();bool active=owner!=NULL;arch_irq_restore(f);return active;}
bool keyboard_raw_read(const void *who,keyboard_raw_state *out){if(!out)return false;uint32_t f=arch_irq_disable();bool ok=owner&&owner==who;if(ok){*out=state;state.count=0;}else memset(out,0,sizeof *out);arch_irq_restore(f);return ok;}
void keyboard_raw_clear(void){uint32_t f=arch_irq_disable();memset(&state,0,sizeof state);memset(matrix,0,sizeof matrix);arch_irq_restore(f);}
uint32_t keyboard_raw_generation(void){uint32_t f=arch_irq_disable();uint32_t n=generation;arch_irq_restore(f);return n;}
void keyboard_raw_ready(uint32_t epoch){uint32_t f=arch_irq_disable();if(owner&&generation==epoch)state.ready=true;arch_irq_restore(f);}
void keyboard_raw_matrix(uint32_t epoch,uint8_t event){
 uint32_t f=arch_irq_disable();unsigned row=(event>>4)&7,col=event&15;
 if(owner&&generation==epoch){
  if(row>=5||col>=14){memset(&state,0,sizeof state);memset(matrix,0,sizeof matrix);}
  else {
   uint8_t keys[32]={0},mods=0;matrix[row*14+col]=!!(event&128);
   bool sym=matrix[42]!=0;
   if(matrix[43]){mods|=2;}if(matrix[56]){mods|=1;}if(matrix[57]){mods|=4;}
   for(unsigned i=0;i<70;i++)if(matrix[i]){
    uint8_t k=base[i];bool shifted=(i==12)||(i>=15&&i<=24)||i==54;
    if(sym){
     if(i==14||i==22||i==25||i==26||i==27||i==54){shifted=!shifted;}
     if(i==15)k=0x38;else if(i==22)k=0x38;else if(i==23)k=0x36;else if(i==24)k=0x37;else if(i==54)k=0x2e;else if(i==65)k=0x36;
    }
    if(shifted){mods|=2;}bit(keys,k);
   }
   publish(keys,mods);
  }
 }
 arch_irq_restore(f);
}
void keyboard_raw_usb(uint32_t epoch,const uint8_t r[8]){
 uint32_t f=arch_irq_disable();
 if(owner&&generation==epoch){
  uint8_t keys[32]={0};bool valid=true;
  for(int i=2;i<8;i++){if(r[i]&&r[i]<4){valid=false;break;}bit(keys,r[i]);}
  if(valid)publish(keys,r[0]);else{memset(&state,0,sizeof state);memset(matrix,0,sizeof matrix);}
 }
 arch_irq_restore(f);
}
