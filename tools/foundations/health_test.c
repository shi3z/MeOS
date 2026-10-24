#include <assert.h>
#include <stdio.h>
#include <m5os/health.h>
#include <m5os/hal_time.h>
#include <m5os/arch.h>
static unsigned depth;static uint64_t now=1000;
uint64_t hal_time_now_us(void){assert(depth==1);return now;}
uint32_t arch_irq_disable(void){return depth++;}
void arch_irq_restore(uint32_t old){assert(depth==old+1);depth=old;}
static uint32_t expired_at(uint64_t value) {
    uint64_t saved=now;now=value;
    uint32_t id=health_expired();now=saved;return id;
}
int main(void) {
    assert(!health_register("",0,1000)&&!health_register("a",0,99)&&!health_register("a",0,60001));
    uint32_t kernel=health_register("service",0,1000),app=health_register("capture",42,100);
    assert(kernel && app && kernel!=app);
    assert(!expired_at(now+99999)&&expired_at(now+100000)==app);
    now+=100000;assert(health_renew(app)&&!expired_at(now));
    health_release_owner(0);assert(health_renew(kernel));
    health_release_owner(42);assert(!health_renew(app));
    uint32_t next=health_register("new",42,100);assert(next && next!=app);
    assert(!health_renew(app)&&!health_renew(0));
    assert(expired_at(now-1)==kernel); /* no underflow when monotonic time goes backwards */
    health_info_t out[HEALTH_MAX];assert(health_snapshot(out,1)==1 && out[0].handle==kernel);
    assert(health_snapshot(NULL,8)==0);
    for(unsigned i=2;i<HEALTH_MAX;i++)assert(health_register("fill",i,1000));
    assert(!health_register("full",99,1000));
    health_release_owner(42);assert(health_register("reuse",43,1000));
    assert(!depth);
    puts("health: timeout boundaries, renewal, stale handles, owner cleanup, table exhaustion and clock reversal PASS");
}
