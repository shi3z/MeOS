#include <m5os/arch.h>

/* See context_switch.S: 32 words, ra at +0, s0 at +4, s1 at +8. */
extern void arch_task_trampoline(void);

#define CTX_FRAME_BYTES  128u

size_t arch_task_context_bytes(void) { return CTX_FRAME_BYTES; }

uint32_t *arch_task_stack_init(void *stack_base, size_t size, void (*entry)(void *), void *arg)
{
    uintptr_t top = ((uintptr_t)stack_base + size) & ~(uintptr_t)(ARCH_STACK_ALIGN - 1);
    uint32_t *sp = (uint32_t *)(top - CTX_FRAME_BYTES);
    for (unsigned i = 0; i < CTX_FRAME_BYTES / 4; i++) sp[i] = 0;
    sp[0] = (uint32_t)(uintptr_t)arch_task_trampoline;     /* ra */
    sp[1] = (uint32_t)(uintptr_t)entry;                    /* s0 */
    sp[2] = (uint32_t)(uintptr_t)arg;                      /* s1 */
    return sp;
}
