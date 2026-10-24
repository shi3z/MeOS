/* Byte-level SD model: CMD24 data/CRC, busy completion, CMD13 and fault latching. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <m5os/sdcard.h>
#include <m5os/hal_sdspi.h>
#include <m5os/errno.h>

static bool selected, v2 = true, power_ok = true;
static unsigned calls, clock_ms, depth, frame_n, qn, qp;
static uint8_t frame[6], queue[1024];
static int fault;
static uint32_t last_addr;
static unsigned write_phase,write_n,write_count;
static uint8_t written[514];
void spi_bus_lock(void) { assert(!depth); depth = 1; }
void spi_bus_unlock(void) { assert(depth == 1); depth = 0; }
uint64_t hal_time_now_us(void) { return (uint64_t)clock_ms++ * 1000; }
void hal_delay_us(uint32_t us) { clock_ms += us / 1000; }
bool sdcard_spi_power_init(void) { return power_ok; }
void hal_sdspi_begin(void) { }
void hal_sdspi_end(void) { }
void hal_sdspi_set_slow(bool s) { (void)s; }
void hal_sdspi_cs(bool s) { selected = s; frame_n = qn = qp = write_phase = 0; }
static void put(uint8_t b) { assert(qn < sizeof queue); queue[qn++] = b; }
static void word(uint32_t v) { for (int i = 24; i >= 0; i -= 8) put(v >> i); }
static void data(const uint8_t *buf, size_t len, bool corrupt)
{
    uint16_t crc = 0;
    put(0xfe);
    for (size_t i = 0; i < len; i++) {
        put(buf[i]); crc ^= (uint16_t)buf[i] << 8;
        for (int b = 0; b < 8; b++) crc = (uint16_t)((crc << 1) ^ ((crc & 0x8000) ? 0x1021 : 0));
    }
    if (corrupt) crc ^= 1;
    put(crc >> 8); put(crc);
}
uint8_t hal_sdspi_xfer(uint8_t out)
{
    assert(depth == 1); calls++;
    if (!selected) return 0xff;
    if (qp < qn) return queue[qp++];
    if(write_phase==3)return 0; /* Simulated stuck programming state. */
    if(write_phase==1){if(out==0xfe){write_phase=2;write_n=0;}else assert(out==0xff);return 0xff;}
    if(write_phase==2){
        written[write_n++]=out;
        if(write_n==514){
            uint16_t crc=0;
            for(unsigned i=0;i<512;i++) {assert(written[i]==(uint8_t)i);crc^=(uint16_t)written[i]<<8;
                for(int b=0;b<8;b++)crc=(uint16_t)((crc<<1)^((crc&0x8000)?0x1021:0));}
            assert(written[512]==(uint8_t)(crc>>8) && written[513]==(uint8_t)crc);
            qn=qp=0;put(fault==10?0x0b:fault==11?0x0d:5);put(0);put(0);put(0xff);
            write_phase=fault==12?3:0;write_count++;
        }
        return 0xff;
    }
    if (!frame_n && (out & 0xc0) != 0x40) return 0xff;
    frame[frame_n++] = out;
    if (frame_n < 6) return 0xff;
    frame_n = qn = qp = 0;
    unsigned cmd = frame[0] & 0x3f;
    uint32_t arg = ((uint32_t)frame[1] << 24) | ((uint32_t)frame[2] << 16) | ((uint32_t)frame[3] << 8) | frame[4];
    uint8_t buf[512] = { 0 };
    switch (cmd) {
    case 0: if (fault != 1) put(1); break;
    case 8: put(v2 ? 1 : 5); if (v2) word(fault == 2 ? 0x1ab : 0x1aa); break;
    case 55: put(fault == 3 ? 5 : 1); break;
    case 41: put(fault == 4 ? 1 : 0); break;
    case 58: put(0); word(fault == 5 ? 0 : (v2 ? 0xc0ff8000 : 0x80ff8000)); break;
    case 16: assert(!v2 && arg == 512); put(0); break;
    case 9:
        put(0);
        if (v2) { buf[0] = 0x40; buf[9] = 7; }
        else { buf[5] = 9; buf[7] = 0xff; buf[8] = 0xc0; }
        if (fault == 7) buf[0] = 0xc0;
        if (fault == 8) { buf[7] = 0x3f; buf[8] = buf[9] = 0xff; }
        if(fault==16)buf[14]=0x30;
        data(buf, 16, fault == 6); break;
    case 17:
        last_addr = arg; put(0);
        for (unsigned i = 0; i < 512; i++) buf[i] = (uint8_t)i;
        data(buf, 512, fault == 9); break;
    case 24: last_addr=arg;put(fault==15?4:0);if(fault!=15)write_phase=1;break;
    case 13:put(fault==13?4:0);put(fault==14?2:0);break;
    default: assert(!"Unexpected SD command (erase is never used)");
    }
    return 0xff;
}
int main(void)
{
    uint8_t buf[1024]; sdcard_status_t s;
    sdcard_status(&s); assert(!s.probed && !s.ready);
    power_ok = false; assert(!sdcard_probe()); power_ok = true;
    for (int f = 1; f <= 8; f++) {
        fault = f; assert(!sdcard_probe()); sdcard_status(&s); assert(s.probed && !s.ready && !depth);
    }
    fault = 0;
    for (int mode = 0; mode < 2; mode++) {
        v2 = mode != 0;
        blockdev_t *bd = sdcard_probe(); assert(bd);
        sdcard_status(&s); assert(s.ready && s.high_capacity == v2);
        assert(s.sectors == (v2 ? 8192 : 4096));
        assert(bd->read(bd, 7, 2, buf) == ERR_OK && last_addr == (v2 ? 8u : 4096u));
        for (unsigned i = 0; i < sizeof buf; i++) assert(buf[i] == (uint8_t)i);
        unsigned before = calls;
        assert(bd->write(bd,s.sectors,1,buf)==ERR_INVAL && calls==before);
        assert(bd->write(bd,1,UINT32_MAX,buf)==ERR_INVAL && calls==before);
        assert(bd->write(bd,0,0,buf)==ERR_INVAL && calls==before);
        assert(bd->write(bd,0,1,NULL)==ERR_INVAL && calls==before);
        assert(bd->read(bd, 1, UINT32_MAX, buf) == ERR_INVAL && calls == before);
        assert(bd->read(bd, s.sectors, 1, buf) == ERR_INVAL && calls == before);
        assert(bd->read(bd, 0, 0, buf) == ERR_INVAL && calls == before);
        unsigned completed=write_count;
        assert(bd->write(bd,7,2,buf)==ERR_OK && last_addr==(v2?8u:4096u));
        assert(write_count==completed+2);
        fault = 9; assert(bd->read(bd, 0, 1, buf) == ERR_IO);
        sdcard_status(&s); assert(!s.ready);
        assert(bd->read(bd, 0, 1, buf) == ERR_NODEV);
        fault = 1; assert(!sdcard_probe()); /* Failed reprobe must not retain ready/capacity. */
        sdcard_status(&s); assert(!s.ready && !s.sectors);
        fault = 0;
    }
    for(int f=10;f<=15;f++) {
        fault=f;blockdev_t *bd=sdcard_probe();assert(bd);
        assert(bd->write(bd,0,1,buf)==ERR_IO);
        sdcard_status(&s);assert(!s.ready && !depth);
        unsigned before=calls;assert(bd->write(bd,0,1,buf)==ERR_NODEV && calls==before);
    }
    fault=16;blockdev_t *bd=sdcard_probe();assert(bd);
    sdcard_status(&s);assert(s.write_protected);
    unsigned before=calls;assert(bd->write(bd,0,1,buf)==ERR_PERM && calls==before);
    puts("SD SPI: SDSC/SDHC init, capacity, CRC, timeouts, bounds, CMD24/CRC, busy/status faults, write protection tests PASS");
}
