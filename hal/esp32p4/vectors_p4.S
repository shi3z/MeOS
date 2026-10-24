/*
 * ESP32-P4 vectored-interrupt table and the CPU tick interrupt entry.
 *
 * P4 interrupt model (read from ESP-IDF v5.4.4: components/riscv/vectors_clic.S, riscv/interrupt_clic.c,
 * hal/interrupt_clic_ll.h, soc/esp32p4/include/soc/clic_reg.h):
 *   - the controller is a CLIC; peripheral sources go through the interrupt matrix onto 32 CPU lines,
 *     which are CLIC interrupt numbers 16..47;
 *   - an interrupt configured "vectored" makes the CPU fetch its handler address from the MTVT table
 *     at  MTVT + 4 * interrupt_number  and jump there;
 *   - MRET lowers the interrupt level to mcause.MPIL.
 *
 * We point MTVT at OUR table (IDF's lives in flash-mapped code and cannot be changed).  The table
 * copies IDF's (vectors_clic.S) except for the tick line, which comes here.  So interrupts that IDF
 * drivers install with esp_intr_alloc() (the DSI display driver needs one to keep its DMA scanning the
 * framebuffer) still reach IDF's _interrupt_handler.  That handler talks to FreeRTOS through
 * rtos_int_enter/exit, which return at once while the scheduler is not running - our situation.
 * The system lines (WDT, cache error, memory protection, assist debug) stay panics.
 */
    .section .iram1, "ax"
    .option push
    .option norvc                       /* keep the table entries and handler 4-byte aligned */

    .global _panic_handler
    .global _interrupt_handler

/* TWO facts about this table, both learned on the hardware:
 *   1. It must live in EXECUTABLE memory: the CLIC reads a vector as an instruction fetch.  Putting
 *      it in data RAM gave "Instruction access fault" with mepc = mtval = the table entry's address.
 *   2. The RAM that holds code is write-protected, so it cannot be filled in at run time (a store
 *      there is a "Store access fault").  Hence the table is built statically, here.
 * M5_TICK_CLIC_ID must equal CLIC_ID in irq.c (checked at run time by arch_tick_init). */
    .equ M5_TICK_CLIC_ID, 36            /* CPU line 20 + 16 */
    .section .iram1, "ax"
    .balign 0x40
    .global m5_mtvt_table
    .type   m5_mtvt_table, @object
m5_mtvt_table:
    .rept 16                            /* 0..15: CLIC system interrupts, never delivered through the table */
    .word _panic_handler
    .endr
    .rept M5_TICK_CLIC_ID - 16          /* 16..35: free lines -> IDF's dispatcher */
    .word _interrupt_handler
    .endr
    .word m5_tick_entry                 /* 36 */
    .rept 40 - M5_TICK_CLIC_ID - 1      /* 37..39 */
    .word _interrupt_handler
    .endr
    .rept 4                             /* 40..43: INT_WDT, cache error, memprot, assist debug: panic like IDF */
    .word _panic_handler
    .endr
    .rept 4                             /* 44..47 */
    .word _interrupt_handler
    .endr
    .size   m5_mtvt_table, . - m5_mtvt_table
/*
 * The tick interrupt.  On entry the hardware has: mepc = interrupted pc, mcause = (interrupt, level,
 * previous level...), MIE = 0 (MPIE = old MIE = 1: the interrupted code had interrupts on or the tick
 * could not have fired).  sp is the interrupted task's stack.
 *
 * Frame, 44 words (integer frame 80 bytes, caller-saved FPU registers
 * at 80..156 and FCSR at 160).
 * Integer frame, 20 words (kept a multiple of 16 bytes): 0 ra | 1..3 t0-t2 | 4..11 a0-a7 | 12..15 t3-t6
 *                                              | 16 mepc | 17 mcause | 18 mstatus
 * Only caller-saved registers are saved here: the C code called below keeps s0-s11, and if the scheduler
 * switches tasks it goes through arch_context_switch(), which saves and restores those.  A task parked
 * inside this handler resumes in the C code, falls out to the epilogue and returns to ITS interrupted
 * code with the epilogue's MRET.
 */
    .balign 4
    .global m5_tick_entry
    .type   m5_tick_entry, @function
m5_tick_entry:
    addi    sp, sp, -176
    sw      ra,   0(sp)
    sw      t0,   4(sp)
    sw      t1,   8(sp)
    sw      t2,  12(sp)
    sw      a0,  16(sp)
    sw      a1,  20(sp)
    sw      a2,  24(sp)
    sw      a3,  28(sp)
    sw      a4,  32(sp)
    sw      a5,  36(sp)
    sw      a6,  40(sp)
    sw      a7,  44(sp)
    sw      t3,  48(sp)
    sw      t4,  52(sp)
    sw      t5,  56(sp)
    sw      t6,  60(sp)
    csrr    t0, mepc
    csrr    t1, mcause
    csrr    t2, mstatus
    sw      t0,  64(sp)
    sw      t1,  68(sp)
    sw      t2,  72(sp)

    li      t0, 0x6000
    csrs    mstatus, t0                 /* allow saves even when interrupting FS=Off code */
    fsw     ft0, 80(sp)
    fsw     ft1, 84(sp)
    fsw     ft2, 88(sp)
    fsw     ft3, 92(sp)
    fsw     ft4, 96(sp)
    fsw     ft5, 100(sp)
    fsw     ft6, 104(sp)
    fsw     ft7, 108(sp)
    fsw     fa0, 112(sp)
    fsw     fa1, 116(sp)
    fsw     fa2, 120(sp)
    fsw     fa3, 124(sp)
    fsw     fa4, 128(sp)
    fsw     fa5, 132(sp)
    fsw     fa6, 136(sp)
    fsw     fa7, 140(sp)
    fsw     ft8, 144(sp)
    fsw     ft9, 148(sp)
    fsw     ft10, 152(sp)
    fsw     ft11, 156(sp)
    frcsr   t0
    sw      t0, 160(sp)
    call    m5_tick_dispatch            /* C: acknowledge, re-arm, run the scheduler (may switch tasks) */

    csrci   mstatus, 8                  /* make sure nothing interrupts the epilogue */
    flw     ft0, 80(sp)
    flw     ft1, 84(sp)
    flw     ft2, 88(sp)
    flw     ft3, 92(sp)
    flw     ft4, 96(sp)
    flw     ft5, 100(sp)
    flw     ft6, 104(sp)
    flw     ft7, 108(sp)
    flw     fa0, 112(sp)
    flw     fa1, 116(sp)
    flw     fa2, 120(sp)
    flw     fa3, 124(sp)
    flw     fa4, 128(sp)
    flw     fa5, 132(sp)
    flw     fa6, 136(sp)
    flw     fa7, 140(sp)
    flw     ft8, 144(sp)
    flw     ft9, 148(sp)
    flw     ft10, 152(sp)
    flw     ft11, 156(sp)
    lw      t0, 160(sp)
    fscsr   t0
    lw      t0,  64(sp)
    lw      t1,  68(sp)
    lw      t2,  72(sp)
    csrw    mepc, t0
    csrw    mcause, t1                  /* CLIC: MRET takes the previous interrupt level from here */
    csrw    mstatus, t2
    lw      ra,   0(sp)
    lw      t0,   4(sp)
    lw      t1,   8(sp)
    lw      t2,  12(sp)
    lw      a0,  16(sp)
    lw      a1,  20(sp)
    lw      a2,  24(sp)
    lw      a3,  28(sp)
    lw      a4,  32(sp)
    lw      a5,  36(sp)
    lw      a6,  40(sp)
    lw      a7,  44(sp)
    lw      t3,  48(sp)
    lw      t4,  52(sp)
    lw      t5,  56(sp)
    lw      t6,  60(sp)
    addi    sp, sp, 176
    mret
    .size   m5_tick_entry, . - m5_tick_entry
    .option pop
