#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <m5os/crash.h>
#include <m5os/app.h>
int main(void) {
    app_t app={.app_id=42};task_t task={.id=7,.app=&app,.stack_size=4096,.stack_guard_failed=true};
    memcpy(task.name,"1234567890123456",16);char longtext[200];memset(longtext,'x',sizeof longtext);longtext[199]=0;
    crash_record_t r;crash_record_make(&r,&task,UINT64_C(1234567890123),longtext,longtext);
    assert(crash_record_valid(&r)&&r.task_id==7&&r.app_id==42&&r.guard_failed);
    assert(r.uptime_us==UINT64_C(1234567890123)&&strlen(r.reason)==79&&strlen(r.build)==23&&strlen(r.task_name)==15);
    crash_record_t copy=r;
    for(size_t i=0;i<sizeof r;i++){((uint8_t *)&r)[i]^=1;assert(!crash_record_valid(&r));r=copy;}
    crash_record_make(&r,NULL,0,NULL,NULL);assert(crash_record_valid(&r)&&!r.task_id&&!r.reason[0]);
    memset(&r,0,sizeof r);assert(!crash_record_valid(&r)&&!crash_record_valid(NULL));
    puts("crash: bounded capture, task/app metadata, full-record corruption rejection and empty boot context PASS");
}
