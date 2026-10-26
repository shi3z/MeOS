#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../../kernel/scheduler/sched.c"
static uint64_t now;
static unsigned depth,switches;
uint64_t hal_time_now_us(void){return now;}
uint32_t arch_irq_disable(void){return depth++;}
void arch_irq_restore(uint32_t old){assert(depth==old+1);depth=old;}
void arch_context_switch(uint32_t **old,uint32_t *next){(void)old;(void)next;switches++;}
void arch_tick_init(uint32_t us,void (*callback)(void)){(void)us;(void)callback;}
void task_stack_check_switch(task_t *task,bool saved){(void)task;(void)saved;}
void kfree(void *p){free(p);}
void msg_free_mailbox(task_t *t){(void)t;}
void msg_task_ended(task_t *t){(void)t;}
void mutex_release_all(task_t *t){(void)t;}
void app_task_ended(task_t *t,app_state_t state){(void)t;(void)state;}
void kernel_panic(const char *s){fprintf(stderr,"%s\n",s);abort();}
int main(void){
 sched_init();
 g_tasks[1]=(task_t){.id=2,.priority=1,.base_priority=1,.state=TASK_RUNNABLE};
 g_tasks[2]=(task_t){.id=3,.priority=1,.base_priority=1,.state=TASK_RUNNABLE};
 now=10;task_yield();assert(task_current()->id==2);
 now=30;task_yield();assert(task_current()->id==3);
 now=40;task_sleep(100);assert(task_current()->id==2);
 task_runtime_info_t out[TASK_MAX];now=45;assert(task_runtime_snapshot(out,TASK_MAX)==3);
 assert(out[0].runtime_us==10&&out[1].runtime_us==25&&out[2].runtime_us==10);
 assert(task_current_runtime_us()==25);
 assert(g_tasks[1].runtime_us==20); /* snapshot is non-mutating */
 now=50;task_yield();assert(task_current()->id==2&&switches==3);
 now=55;task_runtime_snapshot(out,TASK_MAX);assert(out[1].runtime_us==35);
 assert(out[0].runtime_us+out[1].runtime_us+out[2].runtime_us==55);
 assert(task_runtime_snapshot(out,1)==1&&out[0].runtime_us==10);
 assert(task_runtime_snapshot(NULL,1)==0&&task_runtime_snapshot(out,0)==0);
 assert(task_kill(3)==0);now=60;task_yield();
 assert(g_tasks[2].state==TASK_UNUSED);
 task_runtime_snapshot(out,TASK_MAX);assert(out[1].runtime_us==40);
 now=(UINT64_C(1)<<32)+123;assert(task_current_runtime_us()==now-20);
 assert(!depth);
 puts("Scheduler runtime: switches, ongoing interval, same-task yield, snapshot limits and reclaim PASS");
}
