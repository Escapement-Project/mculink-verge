/** SWO capture's ring (src/swo.c): the arithmetic in src/dma_ring.h at this ring's size, and the
 *  USART divisors, free of hardware so tools/swo_ring_test.c can run them on the host.
 *
 * DMA0 writes the trace into SWO_SEGMENTS descriptors without stopping: nothing paces it but the
 * UART. CPU 0's interrupt counts the segments into dap_shared.swo_segments, and CPU 1 reads them.
 */
#ifndef SWO_RING_H
#define SWO_RING_H

#include <stdbool.h>
#include <stdint.h>

#include "dma_ring.h"

#define SWO_SEGMENT_BYTES DMA_RING_SEGMENT_BYTES
#define SWO_SEGMENTS 8U
#define SWO_RING_BYTES (SWO_SEGMENT_BYTES * SWO_SEGMENTS)
#define SWO_USABLE_BYTES (SWO_RING_BYTES - SWO_SEGMENT_BYTES)

/** DMA0's channel for Flexcomm 3 RX. CPU 1 runs it, and CPU 0 counts its completions. */
#define SWO_DMA_CHANNEL 8U
#define SWO_DMA_MASK (1UL << SWO_DMA_CHANNEL)

static inline uint32_t swo_unread(uint32_t written, uint32_t read) {
    return dma_ring_unread(written, read);
}

static inline bool swo_overrun(uint32_t written, uint32_t read) {
    return dma_ring_overrun(written, read, SWO_RING_BYTES);
}

static inline uint32_t swo_resync(uint32_t written) {
    return dma_ring_resync(written, SWO_RING_BYTES);
}

/** Copy `count` bytes from position `read` out of the ring, across its end if need be. */
static inline void swo_copy(uint8_t* out, volatile uint8_t const* ring, uint32_t read, uint32_t count) {
    for (uint32_t i = 0U; i < count; i++) {
        out[i] = ring[(read + i) % SWO_RING_BYTES];
    }
}

/** A USART's divisors for `baud` from a `clock` Hz function clock, and the rate they give.
 *
 * The rate is clock / (oversampling * divider), oversampling 5 to 16 and the divider 1 to 65536.
 * The closest rate, its error measured exactly rather than after the division truncates, and of
 * the closest the one with the most oversampling, which tolerates the most error in the other
 * end's clock. 0, and the divisors left alone, for a rate more than 3% from any it can make: a
 * UART link needs both ends within a few percent between them.
 */
static inline uint32_t swo_uart_divisors(uint32_t clock, uint32_t baud, uint32_t* osrval, uint32_t* brgval) {
    if ((clock == 0U) || (baud == 0U)) {
        return 0U;
    }
    // Errors are fractions, |clock - baud * period| / period, compared by cross-multiplying.
    bool found = false;
    uint64_t best_numerator = 0U;
    uint64_t best_period = 1U;
    for (uint32_t oversampling = 16U; oversampling >= 5U; oversampling--) {
        uint64_t const below = (uint64_t)clock / ((uint64_t)oversampling * baud);
        for (uint64_t divider = below; divider <= below + 1U; divider++) {
            if ((divider < 1U) || (divider > 65536U)) {
                continue;
            }
            uint64_t const period = oversampling * divider;
            uint64_t const made = (uint64_t)baud * period;
            uint64_t const numerator = (made > clock) ? (made - clock) : (clock - made);
            if (!found || (numerator * best_period < best_numerator * period)) {
                found = true;
                best_numerator = numerator;
                best_period = period;
            }
        }
    }
    if (!found || (best_numerator * 100U > (uint64_t)baud * best_period * 3U)) {
        return 0U;
    }
    // The rate depends on the period alone, so of the pairs that make it, the most oversampling.
    for (uint32_t oversampling = 16U; oversampling >= 5U; oversampling--) {
        if ((best_period % oversampling) == 0U) {
            uint64_t const divider = best_period / oversampling;
            if ((divider >= 1U) && (divider <= 65536U)) {
                *osrval = oversampling - 1U;
                *brgval = (uint32_t)divider - 1U;
                break;
            }
        }
    }
    return (uint32_t)((uint64_t)clock / best_period);
}

#endif
