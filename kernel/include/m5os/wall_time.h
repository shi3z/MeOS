#pragma once
#include <m5os/types.h>
typedef struct { unsigned year,month,day,hour,minute,second; } utc_time_t;
typedef enum { WALL_UNSET, WALL_MANUAL, WALL_RTC } wall_source_t;
bool utc_to_epoch(const utc_time_t *utc,uint64_t *epoch);
bool utc_from_epoch(uint64_t epoch,utc_time_t *utc); /* 2000..2099 only */
bool utc_parse(const char *iso,utc_time_t *utc); /* strict YYYY-MM-DDTHH:MM:SSZ */
int wall_time_set(uint64_t epoch,wall_source_t source);
int wall_time_now(uint64_t *epoch,wall_source_t *source);
int rtc_read_epoch(uint64_t *epoch);
int rtc_write_epoch(uint64_t epoch); /* explicit writes only; never changes backup charging */
int time_command(int argc,char **argv);
int rtc_command(int argc,char **argv);
