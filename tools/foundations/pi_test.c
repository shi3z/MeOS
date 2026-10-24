#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <m5os/mutex.h>
#include <m5os/arch.h>
#include "../../kernel/scheduler/sched_internal.h"
task_t g_tasks[TASK_MAX];
static task_t *current;
static unsigned depth;
static jmp_buf blocked;
task_t *task_current(void) { return current; }
uint32_t arch_irq_disable(void) { return depth++; }
void arch_irq_restore(uint32_t old) { assert(depth==old+1); depth=old; }
void kernel_panic(const char *s) { fprintf(stderr,"unexpected panic %s\n",s); abort(); }
void task_yield(void) {}
void sched_reschedule(void) { longjmp(blocked,1); }
static void block_on(task_t *t,mutex_t *m) {
    current=t;
    if(!setjmp(blocked)) { mutex_lock(m); assert(0); }
    assert(t->wait_mutex==m && t->state==TASK_BLOCKED);
    depth=0; /* simulate saved blocked task context; test subsequent independent events */
}
static void init(task_t *t,unsigned id,unsigned prio) {
    *t=(task_t){.id=id,.priority=prio,.base_priority=prio,.state=TASK_RUNNABLE};
}
int main(void) {
    task_t *low=&g_tasks[1],*middle=&g_tasks[2],*high=&g_tasks[3],*other=&g_tasks[4];
    init(low,2,1); init(middle,3,3); init(high,4,7); init(other,5,5);
    mutex_t a=MUTEX_INIT,b=MUTEX_INIT,c=MUTEX_INIT;
    current=low;mutex_lock(&a);mutex_lock(&c);
    current=middle;mutex_lock(&b);block_on(middle,&a);
    assert(low->priority==3);
    block_on(high,&b); /* high -> middle -> low */
    assert(low->priority==7 && middle->priority==7 && high->priority==7);
    block_on(other,&c);
    current=low;mutex_unlock(&a);
    assert(a.owner==middle && !middle->wait_mutex && low->priority==5 && middle->priority==7);
    current=middle;mutex_unlock(&b);
    assert(b.owner==high && !high->wait_mutex && middle->priority==3);
    current=low;mutex_unlock(&c);assert(low->priority==1 && c.owner==other);
    current=middle;mutex_unlock(&a);
    current=other;mutex_unlock(&c);
    current=high;mutex_unlock(&b);
    /* Removing a killed waiter must withdraw its donation. */
    current=low;mutex_lock(&a);block_on(high,&a);assert(low->priority==7);
    high->state=TASK_TERMINATED;mutex_release_all(high);
    assert(low->priority==1 && !high->wait_mutex);
    /* A killed owner hands its mutex to the best waiter and removes donation. */
    block_on(other,&a); low->state=TASK_TERMINATED;mutex_release_all(low);
    assert(a.owner==other && other->state==TASK_RUNNABLE && !low->held);
    current=other;mutex_unlock(&a);
    /* Cycles remain bounded; inheritance is not deadlock detection. */
    init(low,2,1);init(middle,3,3);init(high,4,7);
    current=low;mutex_lock(&a);current=middle;mutex_lock(&b);
    block_on(low,&b);block_on(middle,&a);block_on(high,&a);
    assert(low->priority==7 && middle->priority==7);
    high->state=TASK_TERMINATED;mutex_release_all(high);
    assert(low->priority==3 && middle->priority==3);
    assert(!depth);
    puts("mutex: transitive donation, multiple locks, restore/handoff, killed waiter/owner and cycles PASS");
}
