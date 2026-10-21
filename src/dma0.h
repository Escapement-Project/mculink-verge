/** Reading where a DMA0 ring's writer is, for SWO capture on CPU 1 and the consoles on CPU 0. */
#ifndef DMA0_H
#define DMA0_H

#include <stdbool.h>
#include <stdint.h>

#include "fsl_device_registers.h"

#include "dma_ring.h"

/** Bytes `channel` has written since `*segments` was zero. The count, the pending flag and XFERCFG
 *  are read until the count and the flag read the same on both sides of XFERCFG, so all three
 *  describe one moment. Always inlined, so a caller in SRAM stays there. */
__attribute__((always_inline)) static inline uint32_t dma0_ring_written(volatile uint32_t const* segments,
                                                                        uint32_t channel) {
    uint32_t const mask = 1UL << channel;
    for (;;) {
        uint32_t const counted = *segments;
        __DMB();
        bool const pending = (DMA0->COMMON[0].INTA & mask) != 0U;
        uint32_t const xfercfg = DMA0->CHANNEL[channel].XFERCFG;
        bool const still_pending = (DMA0->COMMON[0].INTA & mask) != 0U;
        __DMB();
        if ((pending == still_pending) && (counted == *segments)) {
            return dma_ring_written(counted, pending, xfercfg);
        }
    }
}

#endif
