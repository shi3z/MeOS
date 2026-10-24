/*
 * Context switch for RISC-V (ESP32-P4).
 *
 * What a parked task's stack looks like - 16 words, pushed by arch_context_switch():
 *     +64..+108 fs0..fs11, +112 FCSR; total frame is 128 bytes.
 *     +0 ra   +4 s0   +8 s1   +12 s2 ... +44 s11   +48..+60 unused (keeps sp 16-byte aligned)
 * Only the callee-saved registers have to be kept: arch_context_switch() is an ordinary function
 * call, so the compiler has already saved whatever else it cares about.
 *
 * WHY THE SWITCH ENDS WITH MRET, NOT RET.
 * The P4's interrupt controller is a CLIC.  Taking an interrupt raises the "current interrupt level"
 * (mintstatus.mil) and only MRET lowers it again (to mcause.mpil) - IDF's own vectors.S says so:
 * "it is mandatory to restore mcause ... When executing mret, the hardware will restore the former
 * threshold".  Now suppose the scheduler is entered from the timer interrupt and picks a task that
 * was parked in ordinary (non-interrupt) code.  A plain RET would run it with the interrupt level
 * still raised, and no further timer interrupt would ever get through.  The ESP32-S3 version never
 * had this problem because Xtensa restores PS (with INTLEVEL) as part of the switch.
 * So every switch resumes the next task through MRET, after setting
 *     mepc  = the return address (where the task parked)
 *     mcause = INT | MPP=machine | MPIL=0      -> level 0, interrupts stay masked, machine mode
 * The INT bit (bit 31) matters: measured on the hardware, MRET only reloads mil from mcause.mpil
 * when mcause says "interrupt".  Without it mil stayed at 0x3F after the first switch and the tick
 * stopped for good.
 * The callers (sched_switch) run with interrupts masked and restore their own mask afterwards,
 * exactly as on the other architectures.
 */
#define MSTATUS_MPIE   0x80
#define MSTATUS_MPP_M  0x1800
#define MCAUSE_INT     (1 << 31)
#define MCAUSE_MPP_M   (3 << 28)

    .section .iram1, "ax"
    .balign 4
    .global arch_context_switch
    .type   arch_context_switch, @function

/* void arch_context_switch(uint32_t **old_sp [a0], uint32_t *new_sp [a1]) */
arch_context_switch:
    addi    sp, sp, -128
    sw      ra,   0(sp)
    sw      s0,   4(sp)
    sw      s1,   8(sp)
    sw      s2,  12(sp)
    sw      s3,  16(sp)
    sw      s4,  20(sp)
    sw      s5,  24(sp)
    sw      s6,  28(sp)
    sw      s7,  32(sp)
    sw      s8,  36(sp)
    sw      s9,  40(sp)
    sw      s10, 44(sp)
    sw      s11, 48(sp)
    li      t0, 0x6000
    csrs    mstatus, t0                 /* FS=Dirty: single-precision FPU enabled */
    fsw     fs0, 64(sp)
    fsw     fs1, 68(sp)
    fsw     fs2, 72(sp)
    fsw     fs3, 76(sp)
    fsw     fs4, 80(sp)
    fsw     fs5, 84(sp)
    fsw     fs6, 88(sp)
    fsw     fs7, 92(sp)
    fsw     fs8, 96(sp)
    fsw     fs9, 100(sp)
    fsw     fs10, 104(sp)
    fsw     fs11, 108(sp)
    frcsr   t0
    sw      t0, 112(sp)
    sw      sp, 0(a0)                  /* *old_sp = sp : "this task is parked here" */

    mv      sp, a1                     /* switch stacks */
    lw      ra,   0(sp)
    lw      s0,   4(sp)
    lw      s1,   8(sp)
    lw      s2,  12(sp)
    lw      s3,  16(sp)
    lw      s4,  20(sp)
    lw      s5,  24(sp)
    lw      s6,  28(sp)
    lw      s7,  32(sp)
    lw      s8,  36(sp)
    lw      s9,  40(sp)
    lw      s10, 44(sp)
    lw      s11, 48(sp)
    flw     fs0, 64(sp)
    flw     fs1, 68(sp)
    flw     fs2, 72(sp)
    flw     fs3, 76(sp)
    flw     fs4, 80(sp)
    flw     fs5, 84(sp)
    flw     fs6, 88(sp)
    flw     fs7, 92(sp)
    flw     fs8, 96(sp)
    flw     fs9, 100(sp)
    flw     fs10, 104(sp)
    flw     fs11, 108(sp)
    lw      t0, 112(sp)
    fscsr   t0
    addi    sp, sp, 128

    csrw    mepc, ra
    li      t0, MSTATUS_MPP_M
    csrs    mstatus, t0                /* MPP = machine */
    li      t0, MSTATUS_MPIE
    csrc    mstatus, t0                /* MPIE = 0: interrupts remain disabled after MRET */
    li      t0, MCAUSE_INT | MCAUSE_MPP_M
    csrw    mcause, t0                 /* interrupt bit set + MPIL = 0: see the note below */
    mret
    .size   arch_context_switch, . - arch_context_switch

/* ------------------------------------------------------------------------- */
/*
 * First entry of a new task: arch_task_stack_init() made its stack look like one parked by
 * arch_context_switch() with  ra = here,  s0 = entry,  s1 = arg.
 */
    .balign 4
    .global arch_task_trampoline
    .type   arch_task_trampoline, @function
arch_task_trampoline:
    csrsi   mstatus, 8                 /* MIE = 1: a task runs with interrupts on */
    mv      a0, s1
    jalr    s0                         /* entry(arg) */
    call    task_exit                  /* entry returned: terminate; never returns */
    .size   arch_task_trampoline, . - arch_task_trampoline
