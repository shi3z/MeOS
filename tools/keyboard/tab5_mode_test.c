#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <m5os/keyboard_raw.h>
#include "../../drivers/tab5kbd/tab5kbd.c"
static int owner,mode=MODE_HID,step,emitted,changes;
static uint8_t events[8],num,at;
static unsigned irq;
static jmp_buf done;
uint32_t arch_irq_disable(void){return irq++;}
void arch_irq_restore(uint32_t old){assert(irq==old+1);irq=old;}
uint64_t hal_time_now_us(void){return (uint64_t)step*10000;}
bool hal_i2c_write_reg(uint8_t address,uint8_t reg,uint8_t value){assert(address==KBD_ADDR);if(reg==REG_MODE){mode=value;changes++;}else if(reg==REG_EVENT_NUM){num=at=0;}return true;}
bool hal_i2c_read_reg(uint8_t address,uint8_t reg,uint8_t *out){assert(address==KBD_ADDR);if(reg==REG_MODE)*out=(uint8_t)mode;else if(reg==REG_EVENT_NUM)*out=num-at;else if(reg==0x20){assert(at<num);*out=events[at++];}else return false;return true;}
bool hal_i2c_read_regs(uint8_t address,uint8_t reg,uint8_t *out,size_t len){assert(address==KBD_ADDR&&reg==REG_HID_EVENT&&len==2&&at<num);out[0]=0;out[1]=events[at++];return true;}
void task_sleep(uint32_t ms){
 assert(ms==POLL_MS);step++;
 keyboard_raw_state state;
 if(step==2){assert(mode==0);events[0]=0xa2;events[1]=0xb4;num=2;at=0;}
 if(step==3){assert(keyboard_raw_read(&owner,&state));assert(state.keys[0x1a/8]&(1u<<(0x1a%8)));assert(state.keys[7/8]&(1u<<(7%8)));events[0]=0x34;num=1;at=0;}
 if(step==4){assert(keyboard_raw_read(&owner,&state));assert(state.keys[0x1a/8]&(1u<<(0x1a%8)));assert(!(state.keys[7/8]&(1u<<(7%8))));assert(!emitted);keyboard_raw_release(&owner);}
 if(step==5){assert(mode==MODE_HID);events[0]=4;num=1;at=0;}
 if(step==6){assert(emitted==1&&changes==2&&!keyboard_raw_active()&&!irq);longjmp(done,1);}
}
void ime_feed(const char *s){assert(!strcmp(s,"a"));emitted++;}
void ime_init(void){}
void hal_i2c_set_pins(int a,int b){(void)a;(void)b;}
void hal_i2c_init(void){}
void kputs(const char *s){(void)s;}
void kprintf(const char *fmt,...){(void)fmt;}
const char *usbkbd_type(void){return "none";}
void usbkbd_stats(keyboard_stats_t *out){(void)out;}
void usbkbd_start(void){}
void osk_start(void){}
int task_create_stack(const char *name,void (*fn)(void *),void *arg,uint8_t priority,size_t stack){(void)name;(void)fn;(void)arg;(void)priority;(void)stack;return 1;}
int main(void){assert(keyboard_raw_claim(&owner));if(!setjmp(done))kbd_task(NULL);puts("Tab5 keyboard: native Normal mode, multi-key/release routing, text suppression and HID/text restoration PASS");}
