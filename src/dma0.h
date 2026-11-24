/** Reading where a DMA0 ring's writer is, for SWO capture on CPU 1 and the consoles on CPU 0. CPU 0
 *  also sets the controller up (src/dma0.c). */
#ifndef DMA0_H
#define DMA0_H

#include <stdbool.h>
#include <stdint.h>

#include "fsl_device_registers.h"

#include "dma_ring.h"

/** The flag DMA0 raises as it completes segment `segment` of a ring's count (src/dma_ring.h). */
static inline volatile uint32_t* dma0_ring_flag(uint32_t segment) {
    return dma_ring_flag_b(segment) ? &DMA0->COMMON[0].INTB : &DMA0->COMMON[0].INTA;
}

/** Bytes `channel` has written since `*segments` was zero. The count, the due segment's flag and
 *  XFERCFG are read until the count and the flag read the same on both sides of XFERCFG, so all
 *  three describe one moment. `*lost` is the next segment's flag, read in the same window: up, the
 *  count is behind by an unknown number of segments and the position means nothing. Always
 *  inlined, so a caller in SRAM stays there. */
__attribute__((always_inline)) static inline uint32_t dma0_ring_written(volatile uint32_t const* segments,
                                                                        uint32_t channel, bool* lost) {
    uint32_t const mask = 1UL << channel;
    for (;;) {
        uint32_t const counted = *segments;
        __DMB();
        volatile uint32_t const* const due = dma0_ring_flag(counted);
        bool const pending = (*due & mask) != 0U;
        uint32_t const xfercfg = DMA0->CHANNEL[channel].XFERCFG;
        bool const still_pending = (*due & mask) != 0U;
        bool const overtaken = (*dma0_ring_flag(counted + 1U) & mask) != 0U;
        __DMB();
        if ((pending == still_pending) && (counted == *segments)) {
            *lost = overtaken;
            return dma_ring_written(counted, pending, xfercfg);
        }
    }
}

/* CPU 0's alone: src/dma0.c, with DMA0's interrupt. */

/** DMA0 is CPU 0's: the consoles use channels 4 to 7, and CPU 1 uses channels 8 and 21 for SWO and
 *  energy capture. Only CPU 0 resets the controller or points it at its table. Its table is in USB
 *  SRAM, so set its security rules before DMA fetches a descriptor. */
void dma0_init(void);

#endif
