#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <m5os/hal_i2c.h>
#include "../../drivers/audio/mic_internal.h"
static uint8_t regs[256];
static unsigned ops, fail_at;
static bool ack(void) { return ++ops != fail_at; }
bool hal_i2c_sys_write_reg(uint8_t addr,uint8_t r,uint8_t v) {
    assert(addr==0x40);
    if (!ack()) return false;
    regs[r]=v; return true;
}
bool hal_i2c_sys_update_reg(uint8_t addr,uint8_t r,uint8_t mask,uint8_t v) {
    assert(addr==0x40 && (r==0x08 || r==0x14 || r==0x15));
    if (!ack()) return false;
    regs[r]=(regs[r]&~mask)|(v&mask); return true;
}
int main(void) {
    mic_levels_t s={0};
    int16_t pcm[]={INT16_MIN,0,123,INT16_MAX, -1,42,-123,-32767};
    assert(mic_levels_add(&s,pcm,sizeof pcm));
    assert(s.frames==2 && s.peak[0]==32768 && s.absolute_sum[0]==32769 && s.clipped[0]==1);
    assert(s.peak[1]==42 && s.absolute_sum[1]==42 && !s.clipped[1]);
    assert(s.peak[2]==123 && s.absolute_sum[2]==246 && !s.clipped[2]);
    assert(s.peak[3]==32767 && s.absolute_sum[3]==65534 && s.clipped[3]==1);
    mic_levels_t before=s;
    assert(!mic_levels_add(&s,pcm,sizeof pcm-1)); assert(!memcmp(&before,&s,sizeof s));
    assert(!mic_levels_add(&s,pcm,0)); assert(!mic_levels_add(&s,NULL,sizeof pcm));
    assert(!mic_levels_add(NULL,pcm,sizeof pcm));
    assert(!mic_levels_add(&s,pcm,MIC_BLOCK_BYTES+8));
    s.frames=UINT32_MAX-1; assert(!mic_levels_add(&s,pcm,sizeof pcm));
    s=(mic_levels_t){0};
    int16_t block[MIC_BLOCK_FRAMES*MIC_SLOTS];
    for (unsigned i=0;i<MIC_BLOCK_FRAMES*MIC_SLOTS;i++) block[i]=INT16_MIN;
    for (unsigned i=0;i<2000;i++) assert(mic_levels_add(&s,block,sizeof block));
    assert(s.frames==512000 && s.absolute_sum[0]==UINT64_C(16777216000) && s.clipped[3]==512000);
    regs[0x08]=0xab; regs[0x14]=0xfb; regs[0x15]=0xa7;
    assert(mic_codec_start()); unsigned start_ops=ops;
    assert(regs[0x08]==0xaa && regs[0x14]==0xf8 && regs[0x15]==0xa4);
    assert(regs[0x12]==2 && regs[0x11]==0x60); /* 4-slot TDM, 16-bit Philips format. */
    assert(regs[0x43]==0x14 && regs[0x46]==0x14 && regs[0x01]==0x20);
    for (unsigned n=1;n<=start_ops;n++) {
        ops=0; fail_at=n;
        assert(!mic_codec_start() && ops==n); /* Initialization stops at the first failed operation. */
        ops=0; fail_at=0; assert(mic_codec_stop() && ops==9);
    }
    for (unsigned n=1;n<=9;n++) {
        ops=0;fail_at=n;assert(!mic_codec_stop() && ops==9); /* Complete best-effort shutdown. */
    }
    ops=0;fail_at=0;assert(mic_codec_start() && mic_codec_stop());
    assert(regs[0x47]==0xff && regs[0x4c]==0xff && regs[0x40]==0xc0 && regs[0x01]==0x7f && regs[0x06]==7);
    puts("mic: PCM slot levels, signed bounds, long capture, malformed blocks, codec failure/stop paths PASS");
}
