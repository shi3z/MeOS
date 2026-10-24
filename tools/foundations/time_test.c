#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <m5os/wall_time.h>
#include <m5os/hal_i2c.h>
#include <m5os/hal_time.h>
#include <m5os/arch.h>
#include <m5os/mutex.h>
#include <m5os/errno.h>
static uint64_t now;
static unsigned depth,ops,fail_at,calendar_reads,rollovers;
static uint8_t regs[256];
static bool stuck_vlf, inject_event;
static hal_i2c_result_t failure=HAL_I2C_TIMEOUT;
uint64_t hal_time_now_us(void){return now;}
uint32_t arch_irq_disable(void){return depth++;}
void arch_irq_restore(uint32_t old){assert(depth==old+1);depth=old;}
void mutex_lock(mutex_t *m){(void)m;assert(!depth);depth=1;}
void mutex_unlock(mutex_t *m){(void)m;assert(depth==1);depth=0;}
hal_i2c_result_t hal_i2c_sys_read_regs_ex(uint8_t addr,uint8_t r,uint8_t *out,size_t n) {
    assert(addr==0x32 && depth==1);
    if(++ops==fail_at)return failure;
    memcpy(out,regs+r,n);
    if(r==0x10 && n==7)calendar_reads++;
    if(r==0x10 && n==1 && rollovers){out[0]^=1;rollovers--;}
    return HAL_I2C_OK;
}
hal_i2c_result_t hal_i2c_sys_write_reg_ex(uint8_t addr,uint8_t r,uint8_t v) {
    assert(addr==0x32 && r!=0x1f && depth==1);
    if(++ops==fail_at)return failure;
    if(r==0x1d) { if(inject_event){regs[r]|=0x10;inject_event=false;} regs[r]&=v; if(stuck_vlf)regs[r]|=2; }
    else regs[r]=v;
    return HAL_I2C_OK;
}
int main(void) {
    uint64_t epoch;utc_time_t t,u;wall_source_t src;
    assert(wall_time_now(&epoch,&src)==ERR_NOENT && src==WALL_UNSET);
    const char *invalid[]={"2000-02-30T00:00:00Z","2100-01-01T00:00:00Z","2026-10-23T24:00:00Z","2026-10-23T00:00:60Z","2026-10-23 00:00:00Z","2026-10-23T00:00:00+09:00"};
    for(unsigned i=0;i<sizeof invalid/sizeof *invalid;i++)assert(!utc_parse(invalid[i],&t));
    assert(utc_parse("2000-02-29T23:59:59Z",&t)&&utc_to_epoch(&t,&epoch));
    assert(utc_from_epoch(epoch,&u)&&!memcmp(&t,&u,sizeof t));
    assert(!utc_from_epoch(946684799,&u)&&!utc_from_epoch(UINT64_C(4102444800),&u));
    for(uint64_t e=946684800;e<=UINT64_C(4102444799);e+=86400) {
        uint64_t back;assert(utc_from_epoch(e,&t)&&utc_to_epoch(&t,&back)&&back==e);
    }
    assert(utc_parse("2026-10-23T12:34:56Z",&t)&&utc_to_epoch(&t,&epoch));
    uint64_t expected=epoch;
    now=123456;assert(wall_time_set(epoch,WALL_MANUAL)==ERR_OK);
    now+=2500000;assert(wall_time_now(&epoch,&src)==ERR_OK && epoch==expected+2 && src==WALL_MANUAL);
    now=1;assert(wall_time_now(&epoch,&src)==ERR_CORRUPT);
    regs[0x1f]=0xa5;regs[0x1e]=0x09;regs[0x1d]=0x8a;
    assert(rtc_write_epoch(expected)==ERR_OK);
    assert(regs[0x1f]==0xa5 && regs[0x1e]==0x09 && regs[0x1d]==0x88);
    assert(rtc_read_epoch(&epoch)==ERR_OK && epoch==expected);
    regs[0x1d]|=2;assert(rtc_read_epoch(&epoch)==ERR_CORRUPT);regs[0x1d]&=~2;
    regs[0x1e]|=0x40;assert(rtc_read_epoch(&epoch)==ERR_CORRUPT);regs[0x1e]&=~0x40;
    regs[0x10]=0x6a;assert(rtc_read_epoch(&epoch)==ERR_CORRUPT);regs[0x10]=0x56;
    regs[0x13]=0x03;assert(rtc_read_epoch(&epoch)==ERR_CORRUPT);
    assert(rtc_write_epoch(expected)==ERR_OK);
    rollovers=1;calendar_reads=0;assert(rtc_read_epoch(&epoch)==ERR_OK && calendar_reads==2);
    rollovers=3;calendar_reads=0;assert(rtc_read_epoch(&epoch)==ERR_CORRUPT && calendar_reads==3);
    ops=0;fail_at=1;assert(rtc_read_epoch(&epoch)==ERR_TIMEOUT);
    ops=0;failure=HAL_I2C_NACK;assert(rtc_read_epoch(&epoch)==ERR_NODEV);
    fail_at=0;ops=0;assert(rtc_write_epoch(expected)==ERR_OK);unsigned write_ops=ops;
    for(unsigned f=1;f<=write_ops;f++) {
        ops=0;fail_at=f;failure=HAL_I2C_TIMEOUT;assert(rtc_write_epoch(expected)==ERR_TIMEOUT);
        fail_at=0;
        if(f>1)assert(rtc_read_epoch(&epoch)==ERR_CORRUPT);
        assert(rtc_write_epoch(expected)==ERR_OK && rtc_read_epoch(&epoch)==ERR_OK && epoch==expected);
    }
    regs[0x1d]=2;inject_event=true;assert(rtc_write_epoch(expected)==ERR_OK && (regs[0x1d]&0x10));
    stuck_vlf=true;assert(rtc_write_epoch(expected)==ERR_CORRUPT && (regs[0x1e]&0x40));
    assert(rtc_read_epoch(&epoch)==ERR_CORRUPT);stuck_vlf=false;assert(rtc_write_epoch(expected)==ERR_OK);
    assert(!depth && regs[0x1f]==0xa5);
    puts("time: century calendar roundtrips, UTC parsing/anchor, RTC validity, rollover, write failures and charging preservation PASS");
}
