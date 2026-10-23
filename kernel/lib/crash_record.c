#include <m5os/crash.h>
#include <m5os/app.h>
#include <string.h>
#define MAGIC 0x4d354352u
static uint32_t checksum(const crash_record_t *r)
{
    const uint8_t *p=(const uint8_t *)r;
    uint32_t hash=2166136261u;
    for (size_t i=0;i<sizeof *r;i++) {
        if (i>=offsetof(crash_record_t,checksum) && i<offsetof(crash_record_t,checksum)+sizeof r->checksum) continue;
        hash=(hash^p[i])*16777619u;
    }
    return hash;
}
static void text(char *out,size_t n,const char *in)
{
    size_t i=0;
    if(in) for(;i+1<n && in[i];i++) out[i]=in[i];
    out[i]=0;
}
void crash_record_make(crash_record_t *r,const task_t *t,uint64_t uptime,const char *build,const char *reason)
{
    memset(r,0,sizeof *r);
    r->schema=CRASH_SCHEMA; r->uptime_us=uptime;
    if(t) {
        r->task_id=t->id; r->saved_sp=(uint32_t)(uintptr_t)t->sp;
        r->stack_bytes=(uint32_t)t->stack_size; r->guard_failed=t->stack_guard_failed;
        r->app_id=t->app ? t->app->app_id : 0;
        text(r->task_name,sizeof r->task_name,t->name);
    }
    text(r->build,sizeof r->build,build); text(r->reason,sizeof r->reason,reason);
    r->magic=MAGIC; r->checksum=checksum(r);
}
bool crash_record_valid(const crash_record_t *r)
{
    return r && r->magic==MAGIC && r->schema==CRASH_SCHEMA && r->checksum==checksum(r) &&
        !r->build[sizeof r->build-1] && !r->task_name[sizeof r->task_name-1] && !r->reason[sizeof r->reason-1];
}
