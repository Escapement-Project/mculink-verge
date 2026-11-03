/* CPU 1's vector table and reset handler.
 *
 * CPU 0 points CPBOOT at the vector table, and the core loads its stack pointer and entry point
 * from the first two words. CPU 1 takes no interrupts, so every other entry is a fault, recorded
 * by core1_fault for CPU 0 to report.
 */
    .syntax unified
    .arch armv8-m.main
    .thumb

    .section .core1_vectors, "a", %progbits
    .align 9
    .global core1_vectors
    .type core1_vectors, %object
core1_vectors:
    .word __core1_stack_top
    .word core1_reset
    .rept 85
    .word core1_default_handler
    .endr
    .size core1_vectors, . - core1_vectors

    .text
    .thumb_func
    .align 2
    .global core1_reset
    .type core1_reset, %function
core1_reset:
    cpsid i
    ldr r0, =core1_vectors
    ldr r1, =0xE000ED08
    str r0, [r1]
    /* CPU 0's stack lies directly below this one: without a limit an overflow runs into it. */
    ldr r0, =__core1_stack_bottom
    msr msplim, r0

    /* .data holds the bit paths that run from RAM as well as initialised variables. */
    ldr r0, =__data_load__
    ldr r1, =__data_start__
    ldr r2, =__data_end__
1:
    cmp r1, r2
    bhs 2f
    ldr r3, [r0], #4
    str r3, [r1], #4
    b 1b
2:
    ldr r1, =__bss_start__
    ldr r2, =__bss_end__
    movs r3, #0
3:
    cmp r1, r2
    bhs 4f
    str r3, [r1], #4
    b 3b
4:
    dsb
    isb
    bl core1_main
    .size core1_reset, . - core1_reset

    .thumb_func
    .align 2
core1_default_handler:
    /* An overflow leaves MSP at the limit, and one push from here would lock the core up before
     * anything is recorded. So read the frame, then give core1_fault the whole stack: it never
     * returns. After an overflow these words are not the frame's, and CFSR.STKOF says so. */
    mrs r3, msp
    ldr r0, [r3, #20]
    ldr r1, [r3, #24]
    ldr r2, [r3, #28]
    ldr r3, =__core1_stack_top
    msr msp, r3
    b core1_fault

    .section .note.GNU-stack,"",%progbits
