#pragma once
#include <m5os/task.h>
#define CRASH_SCHEMA 1u
typedef struct {
    uint32_t magic, schema, checksum, task_id, app_id, saved_sp, stack_bytes, guard_failed;
    uint64_t uptime_us;
    char build[24], task_name[TASK_NAME_MAX], reason[80];
} crash_record_t;
void crash_record_make(crash_record_t *out, const task_t *task, uint64_t uptime, const char *build, const char *reason);
bool crash_record_valid(const crash_record_t *record);
void crash_boot_report(void);
void crash_print_previous(void);
