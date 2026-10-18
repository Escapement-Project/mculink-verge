    .syntax unified
    .arch armv8-m.main

    .section .core1_code, "a", %progbits
    .align 9
    .global core1_vectors
    .type core1_vectors, %object
core1_vectors:
    .word core1_stack + 4096
    .word core1_reset
    .rept 14
    .word core1_default_handler
    .endr
    .rept 71
    .word core1_default_handler
    .endr
    .size core1_vectors, . - core1_vectors

    .section .core1_code.text, "ax", %progbits
    .thumb
    .thumb_func
    .align 2
    .global core1_reset
    .type core1_reset, %function
core1_reset:
    cpsid i
    ldr r0, =core1_vectors
    ldr r1, =0xE000ED08
    str r0, [r1]
    dsb
    isb
    bl dap_core1_main
1:
    wfi
    b 1b
    .size core1_reset, . - core1_reset

    .thumb_func
    .align 2
core1_default_handler:
    b core1_default_handler

    .section .note.GNU-stack,"",%progbits
