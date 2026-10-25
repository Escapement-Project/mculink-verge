/** SWO capture's ring (src/swo.c): the arithmetic in src/dma_ring.h at this ring's size, and the
 *  USART's clock and divisors, free of hardware so tools/swo_ring_test.c can run them on the host.
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
    uint32_t offset = read % SWO_RING_BYTES;
    while (count != 0U) {
        uint32_t run = SWO_RING_BYTES - offset;
        if (run > count) {
            run = count;
        }
        for (uint32_t i = 0U; i < run; i++) {
            out[i] = ring[offset + i];
        }
        out += run;
        count -= run;
        offset = 0U;
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

/** The fractional divider's output limit, UM11126 4.5.49. */
#define SWO_FRG_LIMIT_HZ 100000000U

/** Where SWO's UART takes its function clock from, and the divisors after it. */
typedef struct {
    bool from_main;   // the main clock through the fractional divider; otherwise FRO_HF_DIV
    uint32_t mult;    // the divider's MULT, 0 to 255: main * 256 / (256 + mult)
    uint32_t osrval;
    uint32_t brgval;
} swo_uart_clock_t;

static inline uint32_t swo_frg_output(uint32_t main, uint32_t mult) {
    return (uint32_t)(((uint64_t)main * 256U) / (256U + mult));
}

/** How close a source has to come to be taken before a later one: 0.5%, in thousandths. */
#define SWO_CLOSE_PER_MILLE 5U

typedef struct {
    uint32_t rate;       // 0 for none within 3%
    uint64_t numerator;  // the error is numerator / period, in Hz of function clock per period
    uint64_t period;
    swo_uart_clock_t setup;
} swo_uart_candidate_t;

static inline swo_uart_candidate_t swo_uart_candidate(uint32_t clock, uint32_t baud, bool from_main, uint32_t mult) {
    swo_uart_candidate_t c = {0};
    uint32_t osrval = 0U;
    uint32_t brgval = 0U;
    c.rate = swo_uart_divisors(clock, baud, &osrval, &brgval);
    if (c.rate != 0U) {
        c.period = (uint64_t)(osrval + 1U) * (brgval + 1U);
        uint64_t const made = (uint64_t)baud * c.period;
        c.numerator = (made > clock) ? (made - clock) : (clock - made);
        c.setup = (swo_uart_clock_t){.from_main = from_main, .mult = mult, .osrval = osrval, .brgval = brgval};
    }
    return c;
}

/** Whether `a` is closer to the rate than `b`, a missing candidate never. */
static inline bool swo_uart_closer(swo_uart_candidate_t const* a, swo_uart_candidate_t const* b) {
    return (a->rate != 0U) && ((b->rate == 0U) || (a->numerator * b->period < b->numerator * a->period));
}

static inline bool swo_uart_close(swo_uart_candidate_t const* c, uint32_t baud) {
    return (c->rate != 0U) && (c->numerator * 1000U <= (uint64_t)baud * c->period * SWO_CLOSE_PER_MILLE);
}

/** SWO's clock and divisors for `baud`, and the rate they give: 0 for none within 3%.
 *
 * Three sources, taken in this order by the first within 0.5% of the rate: the `fro` Hz
 * FRO_HF_DIV undivided; the `main` Hz main clock through the fractional divider, which makes main
 * to main / 2 by dropping input clocks, at an output within the divider's limit; and the same
 * above the limit. With none that close, the closest of the three. So the 96 MHz clock serves
 * every rate it makes well, as it always has, and the divider passes its limit only for a rate
 * nothing within it makes within 0.5%: from 150 MHz, above about 20.1 MHz.
 */
static inline uint32_t swo_uart_clock(uint32_t fro, uint32_t main, uint32_t baud, swo_uart_clock_t* setup) {
    swo_uart_candidate_t sources[3];
    sources[0] = swo_uart_candidate(fro, baud, false, 0U);
    sources[1] = (swo_uart_candidate_t){0};
    sources[2] = (swo_uart_candidate_t){0};
    if (!swo_uart_close(&sources[0], baud)) {
        for (uint32_t mult = 0U; mult <= 255U; mult++) {
            uint32_t const clock = swo_frg_output(main, mult);
            swo_uart_candidate_t const c = swo_uart_candidate(clock, baud, true, mult);
            swo_uart_candidate_t* const best = &sources[(clock > SWO_FRG_LIMIT_HZ) ? 2U : 1U];
            if (swo_uart_closer(&c, best)) {
                *best = c;
            }
        }
    }
    uint32_t chosen = 3U;
    for (uint32_t i = 0U; (i < 3U) && (chosen == 3U); i++) {
        if (swo_uart_close(&sources[i], baud)) {
            chosen = i;
        }
    }
    if (chosen == 3U) {
        chosen = 0U;
        for (uint32_t i = 1U; i < 3U; i++) {
            if (swo_uart_closer(&sources[i], &sources[chosen])) {
                chosen = i;
            }
        }
    }
    if (sources[chosen].rate != 0U) {
        *setup = sources[chosen].setup;
    }
    return sources[chosen].rate;
}

#endif
