#pragma once
#include <m5os/task.h>
#define TASK_STACK_FILL 0xa5u
#define TASK_STACK_GUARD 0xd3u
void task_stack_prepare(void *allocation, size_t usable);
bool task_stack_guard_ok(task_t *t); /* caller holds scheduler IRQ lock */
void task_stack_check_switch(task_t *t, bool saved_sp);
