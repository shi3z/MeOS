#include <m5os/wall_time.h>
#include <m5os/hal_time.h>
#include <m5os/arch.h>
#include <m5os/errno.h>
static uint64_t s_epoch,s_anchor;
static wall_source_t s_source;
int wall_time_set(uint64_t epoch,wall_source_t source) {
    utc_time_t value;
    if((source!=WALL_MANUAL && source!=WALL_RTC)||!utc_from_epoch(epoch,&value))return ERR_INVAL;
    uint32_t irq=arch_irq_disable();s_epoch=epoch;s_anchor=hal_time_now_us();s_source=source;arch_irq_restore(irq);return ERR_OK;
}
int wall_time_now(uint64_t *epoch,wall_source_t *source) {
    if(!epoch || !source)return ERR_INVAL;
    uint32_t irq=arch_irq_disable();uint64_t anchor=s_anchor,base=s_epoch,now=hal_time_now_us();wall_source_t src=s_source;arch_irq_restore(irq);
    *source=src;if(src==WALL_UNSET)return ERR_NOENT;
    if(now<anchor)return ERR_CORRUPT;
    uint64_t value=base+(now-anchor)/1000000;utc_time_t utc;
    if(!utc_from_epoch(value,&utc))return ERR_CORRUPT;
    *epoch=value;return ERR_OK;
}
