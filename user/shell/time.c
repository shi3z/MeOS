#include <m5os/wall_time.h>
#include <m5os/kprintf.h>
#include <m5os/task.h>
#include <m5os/errno.h>
#include <string.h>
static void print(uint64_t epoch) {
    utc_time_t t;if(!utc_from_epoch(epoch,&t))return;
    kprintf("%04u-%02u-%02uT%02u:%02u:%02uZ\n",t.year,t.month,t.day,t.hour,t.minute,t.second);
}
int time_command(int argc,char **argv) {
    uint64_t epoch;wall_source_t source;int rc=ERR_INVAL;
    if(argc==1) {
        rc=wall_time_now(&epoch,&source);
        if(rc==ERR_OK){kputs(source==WALL_RTC?"UTC (RTC anchor): ":"UTC (manual anchor): ");print(epoch);return 0;}
        kputs("UTC unavailable; use date sync or date set YYYY-MM-DDTHH:MM:SSZ\n");return 1;
    }
    task_t *task=task_current();if(task && task->app)return 1;
    if(argc==2 && !strcmp(argv[1],"sync")) {
        rc=rtc_read_epoch(&epoch);if(rc==ERR_OK)rc=wall_time_set(epoch,WALL_RTC);
    } else if(argc==3 && !strcmp(argv[1],"set")) {
        utc_time_t t;if(utc_parse(argv[2],&t)&&utc_to_epoch(&t,&epoch))rc=wall_time_set(epoch,WALL_MANUAL);
    }
    if(rc!=ERR_OK)kprintf("date: error %d (RTC is never written by date)\n",rc);
    return rc==ERR_OK?0:1;
}
int rtc_command(int argc,char **argv) {
    uint64_t epoch;int rc=ERR_INVAL;
    if(argc==1){rc=rtc_read_epoch(&epoch);if(rc==ERR_OK)print(epoch);}
    else if(argc==3 && !strcmp(argv[1],"set")) {
        task_t *task=task_current();if(task&&task->app)return 1;
        utc_time_t t;if(utc_parse(argv[2],&t)&&utc_to_epoch(&t,&epoch))rc=rtc_write_epoch(epoch);
    }
    if(rc!=ERR_OK)kprintf("rtc: error %d; set requires YYYY-MM-DDTHH:MM:SSZ; no backup charging changes\n",rc);
    return rc==ERR_OK?0:1;
}
