#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "swo_ring.h"

static uint32_t xfercfg_left(uint32_t left) {
    return ((left - 1U) & SWO_XFERCOUNT_MASK) << SWO_XFERCOUNT_SHIFT;
}

static void test_written_fixed_points(void) {
    assert(swo_written(0U, false, xfercfg_left(1024U)) == 0U);
    assert(swo_written(0U, false, xfercfg_left(1023U)) == 1U);
    assert(swo_written(0U, false, xfercfg_left(1U)) == 1023U);
    // Run out: XFERCOUNT has wrapped to 0x3FF, the same as a whole segment still to go.
    assert(swo_written(0U, true, xfercfg_left(1024U)) == 1024U);
    assert(swo_written(3U, false, xfercfg_left(1000U)) == 3U * 1024U + 24U);
    // Positions are modulo 2^32.
    assert(swo_written(UINT32_MAX / SWO_SEGMENT_BYTES, true, xfercfg_left(1024U)) ==
           (uint32_t)((UINT32_MAX / SWO_SEGMENT_BYTES + 1U) * SWO_SEGMENT_BYTES));
}

/** The DMA, CPU 0's interrupt and CPU 1's double read, interleaved at random. */
struct model {
    uint32_t bytes;     // the truth
    uint32_t left;      // the channel's transfers still to go in this descriptor
    bool pending;       // INTA
    uint32_t segments;  // dap_shared.swo_segments
    int isr;            // 0 idle; 1 has read INTA set; 2 has cleared it, not yet counted
};

static uint32_t rng = 12345U;
static uint32_t next_random(void) {
    rng = rng * 1103515245U + 12345U;
    return rng >> 16;
}

static void step_writer_or_isr(struct model* m) {
    switch (next_random() % 3U) {
        case 0:
            // One byte. At the descriptor's end the flag rises and the next descriptor loads.
            m->bytes++;
            m->left--;
            if (m->left == 0U) {
                m->pending = true;
                m->left = SWO_SEGMENT_BYTES;
            }
            break;
        default:
            if (m->isr == 0 && m->pending) {
                m->isr = 1;
            } else if (m->isr == 1) {
                m->pending = false;
                m->isr = 2;
            } else if (m->isr == 2) {
                m->segments++;
                m->isr = 0;
            }
            break;
    }
}

static void maybe_step(struct model* m) {
    for (uint32_t n = next_random() % 4U; n != 0U; n--) {
        step_writer_or_isr(m);
    }
}

static void test_written_never_long(void) {
    struct model m = {.left = SWO_SEGMENT_BYTES};
    uint32_t accepted = 0U;
    uint32_t short_by_a_segment = 0U;
    for (uint32_t trial = 0U; trial < 2000000U; trial++) {
        uint32_t const before = m.bytes;
        uint32_t const segments = m.segments;
        maybe_step(&m);
        bool const pending = m.pending;
        maybe_step(&m);
        uint32_t const xfercfg = xfercfg_left(m.left);
        maybe_step(&m);
        bool const still = m.pending;
        maybe_step(&m);
        bool const same = (segments == m.segments);
        uint32_t const after = m.bytes;
        maybe_step(&m);
        if ((pending == still) && same) {
            uint32_t const got = swo_written(segments, pending, xfercfg);
            assert((int32_t)(after - got) >= 0);  // never ahead of the writer at the last read
            assert((int32_t)(got - (before - SWO_SEGMENT_BYTES)) >= 0);
            accepted++;
            if ((int32_t)(before - got) > 0) {
                short_by_a_segment++;
            }
        }
    }
    assert(accepted > 1000000U);
    printf("    %u reads accepted, %u of them short\n", accepted, short_by_a_segment);
}

static void test_unread_and_overrun(void) {
    assert(swo_unread(100U, 40U) == 60U);
    assert(swo_unread(40U, 100U) == 0U);  // the writer read short of where the reader is
    assert(swo_unread(5U, UINT32_MAX - 4U) == 10U);
    assert(!swo_overrun(SWO_USABLE_BYTES, 0U));
    assert(swo_overrun(SWO_USABLE_BYTES + 1U, 0U));
    assert(!swo_overrun(3U, UINT32_MAX - SWO_USABLE_BYTES + 4U));
}

static void test_copy_wraps(void) {
    static uint8_t ring[SWO_RING_BYTES];
    for (uint32_t i = 0U; i < SWO_RING_BYTES; i++) {
        ring[i] = (uint8_t)(i * 7U);
    }
    uint8_t out[16];
    uint32_t const from = (5U * SWO_RING_BYTES) - 8U;
    swo_copy(out, ring, from, sizeof(out));
    for (uint32_t i = 0U; i < sizeof(out); i++) {
        assert(out[i] == ring[(from + i) % SWO_RING_BYTES]);
    }
    assert(out[8] == ring[0]);
}

/** Every oversampling and divider, for the period whose rate is closest, measured exactly; then
 *  of the pairs that make that period, the most oversampling. */
static uint32_t best_by_search(uint32_t clock, uint32_t baud, uint32_t* osrval, uint32_t* brgval) {
    bool found = false;
    uint64_t best_numerator = 0U;
    uint64_t best_period = 1U;
    for (uint32_t oversampling = 16U; oversampling >= 5U; oversampling--) {
        for (uint64_t divider = 1U; divider <= 65536U; divider++) {
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
    if (best_numerator * 100U > (uint64_t)baud * best_period * 3U) {
        return 0U;
    }
    for (uint32_t oversampling = 16U; oversampling >= 5U; oversampling--) {
        if ((best_period % oversampling == 0U) && (best_period / oversampling <= 65536U)) {
            *osrval = oversampling - 1U;
            *brgval = (uint32_t)(best_period / oversampling) - 1U;
            break;
        }
    }
    return (uint32_t)(clock / best_period);
}

static void test_divisors(void) {
    uint32_t const clock = 96000000U;
    uint32_t osrval = 99U;
    uint32_t brgval = 99U;
    assert(swo_uart_divisors(clock, 8000000U, &osrval, &brgval) == 8000000U);
    assert(osrval == 11U && brgval == 0U);
    assert(swo_uart_divisors(clock, 6000000U, &osrval, &brgval) == 6000000U);
    assert(osrval == 15U && brgval == 0U);
    assert(swo_uart_divisors(clock, 1000000U, &osrval, &brgval) == 1000000U);
    assert(osrval == 15U && brgval == 5U);
    // 9 MHz is 3.03% from the nearest, 8.727 MHz: refused, the divisors left alone.
    osrval = brgval = 99U;
    assert(swo_uart_divisors(clock, 9000000U, &osrval, &brgval) == 0U);
    assert(osrval == 99U && brgval == 99U);
    assert(swo_uart_divisors(0U, 1000000U, &osrval, &brgval) == 0U);
    assert(swo_uart_divisors(clock, 0U, &osrval, &brgval) == 0U);

    static const uint32_t rates[] = {300U, 9600U, 115200U, 250000U, 921600U, 1000000U, 2000000U, 3000000U,
                                     4000000U, 4800000U, 7000000U, 8000000U, 12000000U, 16000000U};
    for (size_t i = 0U; i < sizeof(rates) / sizeof(rates[0]); i++) {
        uint32_t want_osr = 0U;
        uint32_t want_brg = 0U;
        uint32_t const want = best_by_search(clock, rates[i], &want_osr, &want_brg);
        uint32_t got_osr = 0U;
        uint32_t got_brg = 0U;
        uint32_t const got = swo_uart_divisors(clock, rates[i], &got_osr, &got_brg);
        assert(got == want);
        if (got != 0U) {
            assert(got_osr == want_osr && got_brg == want_brg);
            assert(got == clock / ((got_osr + 1U) * (got_brg + 1U)));
        }
    }
}

/** SWO_Data's reader against a writer that runs at any speed: every byte it hands over is the one
 *  the writer put there, and a reader that falls behind gets data again on its next poll. */
static uint32_t positions[SWO_RING_BYTES];
static uint32_t truth;

static void write_bytes(uint32_t count) {
    for (uint32_t i = 0U; i < count; i++) {
        positions[truth % SWO_RING_BYTES] = truth;
        truth++;
    }
}

/** What swo_written can report: the truth, or a segment short of it. */
static uint32_t observed(void) {
    return ((next_random() % 8U) == 0U && truth >= SWO_SEGMENT_BYTES) ? truth - SWO_SEGMENT_BYTES : truth;
}

static void test_reader_keeps_up_and_recovers(void) {
    uint32_t read = 0U;
    uint64_t delivered = 0U;
    uint32_t overruns = 0U;
    bool behind = false;
    for (uint32_t poll = 0U; poll < 200000U; poll++) {
        // Mostly a reader that keeps up; now and then one that has been away for laps.
        uint32_t const gap = (next_random() % 64U == 0U) ? next_random() % 40000U : next_random() % 600U;
        write_bytes(gap);

        uint32_t const at = observed();
        if (swo_overrun(at, read)) {
            read = swo_resync(at);
            overruns++;
            behind = true;
        }
        uint32_t count = swo_unread(at, read);
        if (count > 508U) {
            count = 508U;
        }
        uint32_t out[508];
        uint32_t const during = next_random() % 64U;  // what the writer adds during one copy
        for (uint32_t i = 0U; i < count; i++) {
            if (i == count / 2U) {
                write_bytes(during);
            }
            out[i] = positions[(read + i) % SWO_RING_BYTES];
        }
        uint32_t const after = observed();
        if (swo_overrun(after, read)) {
            // Only a writer that moved a segment in one copy may pass a reader that just resynced.
            assert(!behind);
            read = swo_resync(after);
            continue;
        }
        for (uint32_t i = 0U; i < count; i++) {
            assert(out[i] == read + i);
        }
        if (behind) {
            assert(count != 0U);
            behind = false;
        }
        read += count;
        delivered += count;
    }
    assert(overruns > 100U);
    printf("    %llu bytes delivered intact across %u overruns\n", (unsigned long long)delivered, overruns);
}

int main(void) {
    test_written_fixed_points();
    test_written_never_long();
    test_unread_and_overrun();
    test_copy_wraps();
    test_reader_keeps_up_and_recovers();
    test_divisors();
    return 0;
}
