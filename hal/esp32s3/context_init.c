#include <m5os/arch.h>

/* See context_switch.S.  The fabricated layout, from the top of the stack:
 *
 *   top      -> +---------------------------+
 *               | (caller's frame: none)    |   a1 of the pseudo-caller = top
 *   top-16   -> +---------------------------+
 *               | base save area (16 bytes) |   caller's a0..a3 as the
 *   top-32   -> +---------------------------+   Window Underflow handler reads them
 *               | [sp+0] saved return addr  |   <- initial sp = top - 32
 *               +---------------------------+
 */
extern void arch_task_trampoline(void);

#define CTX_FRAME_SIZE        32u
#define BASE_SAVE_AREA_SIZE   16u
#define WINDOW_INC_CALL4      (1u << 30)     /* a0[31:30] = 1: return to a CALL4 caller */
#define PC_MASK               0x3fffffffu

size_t arch_task_context_bytes(void) { return CTX_FRAME_SIZE; }

uint32_t *arch_task_stack_init(void *stack_base, size_t size,
                               void (*entry)(void *), void *arg)
{
    uintptr_t top = ((uintptr_t)stack_base + size) & ~(uintptr_t)(ARCH_STACK_ALIGN - 1);
    uint32_t *sp = (uint32_t *)(top - CTX_FRAME_SIZE);

    /* base save area: caller's a0, a1, a2, a3 (order fixed by the underflow handler) */
    uint32_t *save = (uint32_t *)((uintptr_t)sp - BASE_SAVE_AREA_SIZE);
    save[0] = 0;                              /* a0 */
    save[1] = (uint32_t)top;                  /* a1 */
    save[2] = (uint32_t)(uintptr_t)arg;       /* a2 */
    save[3] = (uint32_t)(uintptr_t)entry;     /* a3 */

    /* [sp+0]: what arch_context_switch() loads into a0 before RETW. The top two
     * bits are the window increment; the CPU supplies PC[31:30] itself. */
    sp[0] = ((uint32_t)(uintptr_t)arch_task_trampoline & PC_MASK) | WINDOW_INC_CALL4;
    return sp;
}
