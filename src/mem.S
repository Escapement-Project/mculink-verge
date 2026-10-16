/* memcpy for the Cortex-M33, replacing newlib-nano's byte loop.
 *
 * newlib-nano is built for size, so its memcpy moves one byte an iteration: about 8 cycles a
 * byte from flash. Every reply goes through it on its way into the USB endpoint buffer, which
 * made a 507-byte reply cost 26 us. Here a block of 32 bytes moves through r3-r10 with one ldm
 * and one stm, then words, then bytes.
 *
 * ldm/stm and word accesses need both pointers word aligned: USB RAM is Device memory, where an
 * unaligned access faults. Pointers that share an offset are brought to alignment a byte at a
 * time first; pointers that do not are copied a byte at a time throughout, which none of this
 * firmware's hot copies do.
 *
 * tools/mem_test.py checks it against every length to 600 at every pair of offsets.
 */
    .syntax unified
    .cpu cortex-m33
    .thumb

    .section .text.memcpy, "ax", %progbits
    .global memcpy
    .global __aeabi_memcpy
    .global __aeabi_memcpy4
    .global __aeabi_memcpy8
    .type memcpy, %function
    .type __aeabi_memcpy, %function
    .type __aeabi_memcpy4, %function
    .type __aeabi_memcpy8, %function
    .p2align 2
__aeabi_memcpy:
__aeabi_memcpy4:
__aeabi_memcpy8:
memcpy:
    mov     ip, r0
    eor     r3, r0, r1
    lsls    r3, r3, #30
    bne     .Lbytes

.Lalign:
    tst     r0, #3
    beq     .Laligned
    cbz     r2, .Ldone
    ldrb    r3, [r1], #1
    strb    r3, [r0], #1
    subs    r2, r2, #1
    b       .Lalign

.Laligned:
    cmp     r2, #32
    blo     .Lwords
    push    {r4-r10}
    sub     r2, r2, #32
.Lblock:
    ldmia   r1!, {r3-r10}
    stmia   r0!, {r3-r10}
    subs    r2, r2, #32
    bhs     .Lblock
    add     r2, r2, #32
    pop     {r4-r10}

.Lwords:
    subs    r2, r2, #4
    blo     .Lwords_done
.Lword:
    ldr     r3, [r1], #4
    str     r3, [r0], #4
    subs    r2, r2, #4
    bhs     .Lword
.Lwords_done:
    adds    r2, r2, #4

.Lbytes:
    cbz     r2, .Ldone
.Lbyte:
    ldrb    r3, [r1], #1
    strb    r3, [r0], #1
    subs    r2, r2, #1
    bne     .Lbyte
.Ldone:
    mov     r0, ip
    bx      lr
    .size memcpy, . - memcpy
    .size __aeabi_memcpy, . - __aeabi_memcpy
    .size __aeabi_memcpy4, . - __aeabi_memcpy4
    .size __aeabi_memcpy8, . - __aeabi_memcpy8
