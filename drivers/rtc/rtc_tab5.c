/* Epson RX8130CE, 0x32: time 0x10..0x16, VLF in 0x1d, STOP in 0x1e.
 * No charging/backup register (0x1f) writes. Device qualification is separate. */
#include <m5os/board_config.h>
#include <m5os/wall_time.h>
#include <m5os/errno.h>
#if M5OS_BOARD_TAB5
#include <m5os/hal_i2c.h>
#include <m5os/mutex.h>
static mutex_t s_lock=MUTEX_INIT;
static bool s_tainted;
static int map(hal_i2c_result_t r) { return r==HAL_I2C_OK?ERR_OK:r==HAL_I2C_TIMEOUT?ERR_TIMEOUT:r==HAL_I2C_NACK?ERR_NODEV:ERR_IO; }
static int read(uint8_t reg,uint8_t *data,size_t len) { return map(hal_i2c_sys_read_regs_ex(0x32,reg,data,len)); }
static int write(uint8_t reg,uint8_t data) { return map(hal_i2c_sys_write_reg_ex(0x32,reg,data)); }
static bool bcd(uint8_t v,unsigned *out) { if((v&15)>9 || (v>>4)>9)return false;*out=(v>>4)*10+(v&15);return true; }
static uint8_t encode(unsigned v) {return (uint8_t)((v/10)*16+v%10);}
int rtc_read_epoch(uint64_t *out) {
    if(!out)return ERR_INVAL;
    mutex_lock(&s_lock);int rc=ERR_CORRUPT;
    if(s_tainted)goto done;
    for(unsigned attempt=0;attempt<3;attempt++) {
        uint8_t data[7],before,after,sec,ctrl;
        if((rc=read(0x1d,&before,1)) || (rc=read(0x10,data,7)) ||
            (rc=read(0x10,&sec,1)) || (rc=read(0x1d,&after,1)) || (rc=read(0x1e,&ctrl,1)))goto done;
        rc=ERR_CORRUPT;
        if((before|after)&2 || (ctrl&0xc0))goto done;
        if(sec!=data[0])continue;
        utc_time_t t;
        if(!bcd(data[0],&t.second)||!bcd(data[1],&t.minute)||!bcd(data[2],&t.hour)||
            !bcd(data[4],&t.day)||!bcd(data[5],&t.month)||!bcd(data[6],&t.year))goto done;
        t.year+=2000;
        uint8_t week=data[3];
        if(!week || (week&0x80) || (week&(week-1)) || !utc_to_epoch(&t,out))goto done;
        if(week!=(1u<<((*out/86400+4)%7)))goto done;
        rc=ERR_OK;break;
    }
done:mutex_unlock(&s_lock);return rc;
}
int rtc_write_epoch(uint64_t epoch) {
    utc_time_t t;if(!utc_from_epoch(epoch,&t))return ERR_INVAL;
    uint8_t values[]={encode(t.second),encode(t.minute),encode(t.hour),(uint8_t)(1u<<((epoch/86400+4)%7)),
        encode(t.day),encode(t.month),encode(t.year-2000)};
    mutex_lock(&s_lock);uint8_t ctrl,flags;int rc;
    if((rc=read(0x1e,&ctrl,1)))goto done;
    if(ctrl&0x80){rc=ERR_CORRUPT;goto done;}
    s_tainted=true;
    if((rc=write(0x1e,ctrl|0x40)))goto done;
    for(unsigned i=0;i<7;i++)if((rc=write(0x10+i,values[i])))goto done;
    /* Flag bits are write-zero-to-clear: write ones to preserve unrelated
     * events, including events arriving after our read. Bit 6 is reserved zero. */
    if((rc=write(0x1d,0xbd)) || (rc=read(0x1d,&flags,1)))goto done;
    if(flags&2){rc=ERR_CORRUPT;goto done;} /* oscillator not stable: VLF did not clear */
    if((rc=write(0x1e,ctrl&~0x40u)))goto done;
    s_tainted=false;
done:mutex_unlock(&s_lock);return rc;
}
#else
int rtc_read_epoch(uint64_t *epoch) { (void)epoch;return ERR_NOSYS; }
int rtc_write_epoch(uint64_t epoch) { (void)epoch;return ERR_NOSYS; }
#endif
