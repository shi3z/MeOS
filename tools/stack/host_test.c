#include <assert.h>
#include <stdarg.h>
#include <setjmp.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <m5os/task.h>
#include <m5os/arch.h>
#include <m5os/app.h>
#include <m5os/errno.h>
#include "../../kernel/task/stack_internal.h"
task_t g_tasks[TASK_MAX];
static unsigned irq_depth, restores, replace_after, allocations, frees;
static bool allow = true, fail_alloc;
static jmp_buf panic_jmp;
static bool expect_panic;
static task_t *current = &g_tasks[0];
task_t *task_current(void) { return current; }
bool task_has_perm(uint32_t perm) { assert(perm == APP_PERM_SPAWN); return allow; }
void *kmalloc_internal(size_t n) { allocations++; return fail_alloc ? NULL : aligned_alloc(16, (n+15)&~(size_t)15); }
void kfree(void *p) { frees++; free(p); }
void kprintf(const char *fmt, ...) { (void)fmt; }
int ksnprintf(char *out,size_t cap,const char *fmt,...) { va_list a;va_start(a,fmt);int n=vsnprintf(out,cap,fmt,a);va_end(a);return n; }
const char *task_state_name(task_state_t state) { (void)state; return "test"; }
void arch_task_trampoline(void) {}
uint32_t arch_irq_disable(void) { return irq_depth++; }
void arch_irq_restore(uint32_t old) {
    assert(irq_depth == old+1); irq_depth = old; restores++;
    if (replace_after && restores == replace_after) {
        /* Simulate a tick reclaiming and reusing the target's TCB between chunks. */
        kfree(g_tasks[1].stack_base); g_tasks[1] = (task_t){ .id=999, .state=TASK_RUNNABLE };
    }
}
void kernel_panic(const char *msg) {
    (void)msg; assert(expect_panic); longjmp(panic_jmp, 1);
}
static void entry(void *p) { (void)p; }
int main(void) {
    current->id=1; current->state=TASK_RUNNING;
    task_stack_info_t info;
    assert(task_stack_inspect(1,&info)==ERR_NOSYS);
    assert(task_stack_inspect(123,&info)==ERR_NOENT);
    assert(task_stack_inspect(1,NULL)==ERR_INVAL);
    unsigned before=allocations;
    assert(task_create_stack("small",entry,NULL,1,511)<0);
    assert(task_create_stack("overflow",entry,NULL,1,(size_t)-1)<0);
    assert(task_create_stack("entry",NULL,NULL,1,1024)<0);
    assert(task_create_stack("prio",entry,NULL,8,1024)<0);
    assert(allocations==before);
    allow=false; assert(task_create("denied",entry,NULL,1)==ERR_PERM); allow=true;
    fail_alloc=true; assert(task_create("oom",entry,NULL,1)<0); fail_alloc=false;
    g_tasks[1].runtime_us=12345; /* reused TCB must not inherit accounting */
    int pid=task_create_stack("probe",entry,NULL,1,1027); assert(pid>1);
    task_t *t=&g_tasks[1]; assert(t->id==(unsigned)pid && t->stack_size==1024);
    assert(!t->runtime_us);
    assert(task_stack_inspect(pid,&info)==ERR_OK && info.guard_ok);
    assert(info.used>0 && info.used<=128 && info.size==1024 && info.free+info.used==1024);
    task_stack_check_switch(t,true);
    uint8_t *low=(uint8_t *)t->stack_base+TASK_STACK_GUARD_BYTES;
    low[200]=0; assert(task_stack_inspect(pid,&info)==ERR_OK && info.used==824 && info.free==200);
    low[5]=0; assert(task_stack_inspect(pid,&info)==ERR_OK && info.used==1019);
    ((uint8_t *)t->stack_base)[7]=0;
    assert(task_stack_inspect(pid,&info)==ERR_OK && !info.guard_ok);
    ((uint8_t *)t->stack_base)[7]=TASK_STACK_GUARD;
    assert(!task_stack_guard_ok(t)); /* Damage stays latched even if bytes are restored. */
    expect_panic=true;
    if (!setjmp(panic_jmp)) { task_stack_check_switch(t,false); assert(0); }
    t->stack_guard_failed=false;
    uint32_t *sp=t->sp; t->sp=(uint32_t *)(low-16);
    if (!setjmp(panic_jmp)) { task_stack_check_switch(t,true); assert(0); }
    t->sp=(uint32_t *)(low+t->stack_size-16);
    if (!setjmp(panic_jmp)) { task_stack_check_switch(t,true); assert(0); }
    t->sp=(uint32_t *)(low+1);
    if (!setjmp(panic_jmp)) { task_stack_check_switch(t,true); assert(0); }
    t->sp=sp; expect_panic=false;
    task_stack_prepare(t->stack_base,t->stack_size);
    restores=0; replace_after=2;
    assert(task_stack_inspect(pid,&info)==ERR_NOENT && irq_depth==0);
    replace_after=0;
    /* A reused slot gets fresh guards and no latched error. */
    g_tasks[1].state=TASK_UNUSED; g_tasks[1].stack_guard_failed=true;
    pid=task_create_stack("reuse",entry,NULL,1,512); assert(pid>1);
    assert(task_stack_inspect(pid,&info)==ERR_OK && info.guard_ok && info.size==512);
    kfree(g_tasks[1].stack_base); g_tasks[1].state=TASK_UNUSED;
    for(int i=1;i<TASK_MAX;i++) g_tasks[i].state=TASK_RUNNABLE;
    before=frees; assert(task_create("full",entry,NULL,1)<0 && frees==before+1);
    assert(!irq_depth);
    puts("stack: real context frames, watermark, guard latch, SP bounds, reuse/reclaim and create failures PASS");
}
