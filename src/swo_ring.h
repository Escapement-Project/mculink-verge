/** The arithmetic behind SWO capture (src/swo.c), free of hardware so tools/swo_ring_test.c can
 *  run it on the host.
 *
 * DMA0 writes the trace into a ring of SWO_SEGMENTS descriptors of SWO_SEGMENT_BYTES each, without
 * stopping: nothing paces it but the UART. Byte n of the stream lands at ring[n % SWO_RING_BYTES].
 * Positions are byte counts since capture started, modulo 2^32, so every comparison is a difference.
 */
#ifndef SWO_RING_H
#define SWO_RING_H

#include <stdbool.h>
#include <stdint.h>

/** A descriptor's maximum: XFERCOUNT is ten bits, minus one encoded. */
#define SWO_SEGMENT_BYTES 1024U
#define SWO_SEGMENTS 8U
#define SWO_RING_BYTES (SWO_SEGMENT_BYTES * SWO_SEGMENTS)

/** How much of the ring a reader can trust. swo_written can read one segment short, so a reader
 *  that trusted the whole ring could take bytes the DMA had already overwritten. */
#define SWO_USABLE_BYTES (SWO_RING_BYTES - SWO_SEGMENT_BYTES)

/** DMA0's channel for Flexcomm 3 RX. CPU 1 runs it, and CPU 0 counts its completions. */
#define SWO_DMA_CHANNEL 8U
#define SWO_DMA_MASK (1UL << SWO_DMA_CHANNEL)

/** XFERCFG's XFERCOUNT field. */
#define SWO_XFERCOUNT_SHIFT 16U
#define SWO_XFERCOUNT_MASK 0x3FFU

/** Bytes written since capture started.
 *
 * `segments` is how many descriptors CPU 0 has seen complete, `pending` whether one has completed
 * that it has not counted yet, and `xfercfg` the channel's XFERCFG. The three have to come from
 * one moment, which swo.c ensures by reading them twice. CPU 0 clears the completion before it
 * counts it, so a read between the two finds neither and comes out one segment short, never long.
 * A descriptor that has just run out reads as a full one still to go, which comes to the same
 * position as the next descriptor not yet started.
 */
static inline uint32_t swo_written(uint32_t segments, bool pending, uint32_t xfercfg) {
    uint32_t const left = ((xfercfg >> SWO_XFERCOUNT_SHIFT) & SWO_XFERCOUNT_MASK) + 1U;
    return ((segments + (pending ? 1U : 0U)) * SWO_SEGMENT_BYTES) + (SWO_SEGMENT_BYTES - left);
}

/** Bytes between the reader and the writer, or none when the writer reads behind the reader
 *  (swo_written being short). */
static inline uint32_t swo_unread(uint32_t written, uint32_t read) {
    int32_t const ahead = (int32_t)(written - read);
    return (ahead > 0) ? (uint32_t)ahead : 0U;
}

/** Has the writer come close enough round the ring that bytes from `read` on may be gone? */
static inline bool swo_overrun(uint32_t written, uint32_t read) {
    return swo_unread(written, read) > SWO_USABLE_BYTES;
}

/** Where a reader that the writer has passed starts again: the newest bytes, less two segments of
 *  room, one for `written` having read a segment short and one for the writer to move before the
 *  reader's next check. With less, the copy that follows can count as another overrun, and so can
 *  every one after it. */
static inline uint32_t swo_resync(uint32_t written) {
    return written - (SWO_USABLE_BYTES - (2U * SWO_SEGMENT_BYTES));
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
