/* Bare-metal harness for QEMU's mps2-an505 (Cortex-M33): runs the sweep, prints over semihosting. */
#include <stdint.h>
extern uint32_t __stack_top, __data_lma, __data_start, __data_end, __bss_start, __bss_end;
void sweep(void);

static int semihost(int op, const void* arg) {
    register int r0 __asm__("r0") = op;
    register const void* r1 __asm__("r1") = arg;
    __asm__ volatile("bkpt 0xab" : "+r"(r0) : "r"(r1) : "memory");
    return r0;
}
void out(const char* s) { semihost(0x04, s); }

__attribute__((noreturn)) void reset(void) {
    for (uint32_t *s = &__data_lma, *d = &__data_start; d < &__data_end;) *d++ = *s++;
    for (uint32_t* d = &__bss_start; d < &__bss_end;) *d++ = 0;
    sweep();
    semihost(0x18, (const void*)0x20026); /* ADP_Stopped_ApplicationExit */
    for (;;) {}
}
__attribute__((noreturn)) static void fault(void) {
    out("FAULT\n");
    semihost(0x18, (const void*)0x20023); /* ADP_Stopped_RunTimeErrorUnknown */
    for (;;) {}
}
__attribute__((section(".vectors"), used)) const void* const vectors[16] = {
    &__stack_top, reset, fault, fault, fault, fault, fault, 0, 0, 0, 0, fault, fault, 0, fault, fault,
};
