/* The host side of tools/gen_host.py: SRAMX and the GPIO block mapped where the generator expects them. */
#include <stdio.h>
#include <sys/mman.h>
#include "DAP_config.h"
#include "swd_gen.h"
void sweep(void);
void out(const char* s) { fputs(s, stdout); }
int main(void) {
    if (mmap((void*)SWD_GEN_BASE, 0x8000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) != (void*)SWD_GEN_BASE) return 1;
    if (mmap((void*)GPIO_ADDR, 0x4000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) != (void*)GPIO_ADDR) return 1;
    sweep();
    return 0;
}
