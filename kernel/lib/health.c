#include <m5os/health.h>
#include <m5os/arch.h>
#include <m5os/hal_time.h>
static health_info_t s_leases[HEALTH_MAX];
static uint32_t s_next=1;
uint32_t health_register(const char *name,uint32_t owner,uint32_t timeout) {
    if(!name || !*name || timeout<100 || timeout>60000)return 0;
    uint32_t irq=arch_irq_disable(),id=0;
    if(s_next) for(unsigned i=0;i<HEALTH_MAX;i++)if(!s_leases[i].handle) {
        health_info_t *p=&s_leases[i];*p=(health_info_t){.handle=s_next++,.owner=owner,.timeout_ms=timeout,.last_us=hal_time_now_us()};
        for(unsigned j=0;j<sizeof p->name-1 && name[j];j++)p->name[j]=name[j];
        id=p->handle;break;
    }
    arch_irq_restore(irq);return id;
}
bool health_renew(uint32_t id) {
    uint32_t irq=arch_irq_disable();bool found=false;
    if(id)for(unsigned i=0;i<HEALTH_MAX;i++)if(s_leases[i].handle==id) {s_leases[i].last_us=hal_time_now_us();found=true;break;}
    arch_irq_restore(irq);return found;
}
void health_release_owner(uint32_t owner) {
    if(!owner)return; /* kernel services cannot be removed by an app cleanup */
    uint32_t irq=arch_irq_disable();for(unsigned i=0;i<HEALTH_MAX;i++)if(s_leases[i].owner==owner)s_leases[i].handle=0;arch_irq_restore(irq);
}
uint32_t health_expired(void) {
    uint32_t irq=arch_irq_disable(),id=0;
    uint64_t now=hal_time_now_us(); /* same critical section as renewal timestamps */
    for(unsigned i=0;i<HEALTH_MAX;i++)if(s_leases[i].handle &&
        (now<s_leases[i].last_us || now-s_leases[i].last_us>=(uint64_t)s_leases[i].timeout_ms*1000)) {id=s_leases[i].handle;break;}
    arch_irq_restore(irq);return id;
}
unsigned health_snapshot(health_info_t *out,unsigned max) {
    if(!out)return 0;
    uint32_t irq=arch_irq_disable();unsigned n=0;
    for(unsigned i=0;i<HEALTH_MAX && n<max;i++)if(s_leases[i].handle)out[n++]=s_leases[i];
    arch_irq_restore(irq);return n;
}
