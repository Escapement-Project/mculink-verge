#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "swo_ring.h"

/** src/vcom.c's receive ring. */
#define CONSOLE_RING_BYTES (4U * DMA_RING_SEGMENT_BYTES)

static uint32_t xfercfg_left(uint32_t left) {
    return ((left - 1U) & DMA_XFERCOUNT_MASK) << DMA_XFERCOUNT_SHIFT;
}

static void test_written_fixed_points(void) {
    assert(dma_ring_written(0U, false, xfercfg_left(1024U)) == 0U);
    assert(dma_ring_written(0U, false, xfercfg_left(1023U)) == 1U);
    assert(dma_ring_written(0U, false, xfercfg_left(1U)) == 1023U);
    // Run out: XFERCOUNT has wrapped to 0x3FF, the same as a whole segment still to go.
    assert(dma_ring_written(0U, true, xfercfg_left(1024U)) == 1024U);
    assert(dma_ring_written(3U, false, xfercfg_left(1000U)) == 3U * 1024U + 24U);
    // Positions are modulo 2^32.
    assert(dma_ring_written(UINT32_MAX / SWO_SEGMENT_BYTES, true, xfercfg_left(1024U)) ==
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
            uint32_t const got = dma_ring_written(segments, pending, xfercfg);
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
    assert(dma_ring_usable(SWO_RING_BYTES) == SWO_USABLE_BYTES);
    assert(!dma_ring_overrun(3U * 1024U, 0U, CONSOLE_RING_BYTES));
    assert(dma_ring_overrun((3U * 1024U) + 1U, 0U, CONSOLE_RING_BYTES));
    assert(dma_ring_resync(10000U, CONSOLE_RING_BYTES) == 10000U - 1024U);
    // A short read is behind the reader by under a segment; more is a writer 2^31 or more ahead.
    assert(!dma_ring_overrun(100U, 100U + SWO_SEGMENT_BYTES, SWO_RING_BYTES));
    assert(dma_ring_overrun(100U, 101U + SWO_SEGMENT_BYTES, SWO_RING_BYTES));
    assert(dma_ring_overrun(0x80000000U, 0U, CONSOLE_RING_BYTES));
    assert(dma_ring_overrun(0x80000001U, 0U, CONSOLE_RING_BYTES));
    assert(dma_ring_overrun(UINT32_MAX - SWO_SEGMENT_BYTES, 0U, CONSOLE_RING_BYTES));
}

/** A console's reader, src/vcom.c receive(): it counts the completions on its own core, so its
 *  positions are exact and it does not check again after copying. The writer moves less than a
 *  segment during a copy, and the reader can be away for any number of bytes, past 2^32 included. */
static void test_console_reader(void) {
    static uint32_t ring[CONSOLE_RING_BYTES];
    uint32_t writer = 0U;
    uint32_t read = 0U;
    uint64_t delivered = 0U;
    uint32_t resyncs = 0U;
    for (uint32_t poll = 0U; poll < 200000U; poll++) {
        uint32_t gap = next_random() % 2000U;
        if (next_random() % 256U == 0U) {
            gap = (next_random() << 16) ^ next_random();  // away for up to 2^32 bytes
        }
        // Only the ring's last lap matters to what it holds.
        uint32_t const skip = (gap > CONSOLE_RING_BYTES) ? gap - CONSOLE_RING_BYTES : 0U;
        writer += skip;
        for (uint32_t i = skip; i < gap; i++) {
            ring[writer % CONSOLE_RING_BYTES] = writer;
            writer++;
        }
        uint32_t const at = writer;
        if (dma_ring_overrun(at, read, CONSOLE_RING_BYTES)) {
            read = dma_ring_resync(at, CONSOLE_RING_BYTES);
            resyncs++;
        }
        uint32_t count = dma_ring_unread(at, read);
        if (count > 512U) {
            count = 512U;
        }
        // vcom_ready wakes on at != read. Nothing to deliver then is only a writer that lapped the
        // reader by just under 2^32, which less than a segment more turns into an overrun.
        assert(count != 0U || at == read || (read - at) <= DMA_RING_SEGMENT_BYTES);
        uint32_t const during = next_random() % DMA_RING_SEGMENT_BYTES;
        for (uint32_t i = 0U; i < count; i++) {
            if (i == count / 2U) {
                for (uint32_t k = 0U; k < during; k++) {
                    ring[writer % CONSOLE_RING_BYTES] = writer;
                    writer++;
                }
            }
            assert(ring[(read + i) % CONSOLE_RING_BYTES] == read + i);
        }
        read += count;
        delivered += count;
    }
    assert(resyncs > 100U);
    printf("    console reader: %llu bytes delivered intact across %u resyncs\n", (unsigned long long)delivered,
           resyncs);
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

/** A reader against a writer that runs at any speed, for SWO_Data's ring and a console's: every
 *  byte it hands over is the one the writer put there, and a reader that falls behind gets data
 *  again on its next poll. */
static uint32_t positions[SWO_RING_BYTES];
static uint32_t truth;
static uint32_t ring_bytes;

static void write_bytes(uint32_t count) {
    for (uint32_t i = 0U; i < count; i++) {
        positions[truth % ring_bytes] = truth;
        truth++;
    }
}

/** What SWO_Data's position can report: the truth, or a segment short of it. */
static uint32_t observed(void) {
    return ((next_random() % 8U) == 0U && truth >= SWO_SEGMENT_BYTES) ? truth - SWO_SEGMENT_BYTES : truth;
}

static void test_reader_keeps_up_and_recovers(uint32_t ring) {
    ring_bytes = ring;
    truth = 0U;
    uint32_t read = 0U;
    uint64_t delivered = 0U;
    uint32_t overruns = 0U;
    bool behind = false;
    for (uint32_t poll = 0U; poll < 200000U; poll++) {
        // Mostly a reader that keeps up; now and then one that has been away for laps.
        uint32_t const gap = (next_random() % 64U == 0U) ? next_random() % 40000U : next_random() % 600U;
        write_bytes(gap);

        uint32_t const at = observed();
        if (dma_ring_overrun(at, read, ring_bytes)) {
            read = dma_ring_resync(at, ring_bytes);
            overruns++;
            behind = true;
        }
        uint32_t count = dma_ring_unread(at, read);
        if (count > 508U) {
            count = 508U;
        }
        uint32_t out[508];
        uint32_t const during = next_random() % 64U;  // what the writer adds during one copy
        for (uint32_t i = 0U; i < count; i++) {
            if (i == count / 2U) {
                write_bytes(during);
            }
            out[i] = positions[(read + i) % ring_bytes];
        }
        uint32_t const after = observed();
        if (dma_ring_overrun(after, read, ring_bytes)) {
            // Only a writer that moved a segment in one copy may pass a reader that just resynced.
            assert(!behind);
            read = dma_ring_resync(after, ring_bytes);
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
    printf("    %u-byte ring: %llu bytes delivered intact across %u overruns\n", ring_bytes,
           (unsigned long long)delivered, overruns);
}

/** The clock choice: rates the 96 MHz clock makes stay on it, the rest come from 150 MHz through
 *  the fractional divider, within its limit whenever any output there is within 3%. */
static void test_uart_clock(void) {
    uint32_t const fro = 96000000U;
    uint32_t const main_clock = 150000000U;
    swo_uart_clock_t got;
    // The CC1312's 48 MHz / (ACPR + 1): all but 24 from the 96 MHz clock, as before.
    static const uint32_t direct[] = {8000000U, 9600000U, 12000000U, 16000000U, 1000000U};
    for (size_t i = 0U; i < sizeof(direct) / sizeof(direct[0]); i++) {
        uint32_t osrval = 0U;
        uint32_t brgval = 0U;
        assert(swo_uart_clock(fro, main_clock, direct[i], &got) == swo_uart_divisors(fro, direct[i], &osrval, &brgval));
        assert(!got.from_main && got.mult == 0U && got.osrval == osrval && got.brgval == brgval);
    }
    // 24 MHz: 150 x 256 / 320 = 120 MHz, past the limit, oversampling 5.
    assert(swo_uart_clock(fro, main_clock, 24000000U, &got) == 24000000U);
    assert(got.from_main && got.mult == 64U && got.osrval == 4U && got.brgval == 0U);
    // 20 MHz: exactly the limit.
    assert(swo_uart_clock(fro, main_clock, 20000000U, &got) == 20000000U);
    assert(got.from_main && swo_frg_output(main_clock, got.mult) == SWO_FRG_LIMIT_HZ);
    // 30 MHz is the main clock undivided; past that nothing is within 3%.
    assert(swo_uart_clock(fro, main_clock, 30000000U, &got) == 30000000U && got.mult == 0U);
    assert(swo_uart_clock(fro, main_clock, 31000000U, &got) == 0U);
    assert(swo_uart_clock(0U, 0U, 1000000U, &got) == 0U);

    // Every rate from 1 to 30 MHz in 10 kHz steps, against every source there is.
    uint32_t first_above = 0U;
    for (uint32_t baud = 1000000U; baud <= 30000000U; baud += 10000U) {
        uint32_t const rate = swo_uart_clock(fro, main_clock, baud, &got);
        assert(rate != 0U);
        uint32_t const clock = got.from_main ? swo_frg_output(main_clock, got.mult) : fro;
        assert(rate == clock / ((got.osrval + 1U) * (got.brgval + 1U)));
        swo_uart_candidate_t const chosen = swo_uart_candidate(clock, baud, got.from_main, got.mult);
        assert(chosen.rate == rate);
        // Some source makes every rate here within 0.5%, the divider's steps being that fine.
        assert(chosen.numerator * 200U <= (uint64_t)baud * chosen.period);
        swo_uart_candidate_t const direct = swo_uart_candidate(fro, baud, false, 0U);
        swo_uart_candidate_t within = {0};
        swo_uart_candidate_t beyond = {0};
        for (uint32_t mult = 0U; mult <= 255U; mult++) {
            uint32_t const output = swo_frg_output(main_clock, mult);
            swo_uart_candidate_t const c = swo_uart_candidate(output, baud, true, mult);
            swo_uart_candidate_t* const best = (output > SWO_FRG_LIMIT_HZ) ? &beyond : &within;
            if (swo_uart_closer(&c, best)) {
                *best = c;
            }
        }
        if (swo_uart_close(&direct, baud)) {
            assert(!got.from_main);
        } else if (swo_uart_close(&within, baud)) {
            assert(got.from_main && clock <= SWO_FRG_LIMIT_HZ && !swo_uart_closer(&within, &chosen));
        } else if (swo_uart_close(&beyond, baud)) {
            assert(got.from_main && clock > SWO_FRG_LIMIT_HZ && !swo_uart_closer(&beyond, &chosen));
        } else {
            assert(!swo_uart_closer(&direct, &chosen) && !swo_uart_closer(&within, &chosen) &&
                   !swo_uart_closer(&beyond, &chosen));
        }
        if (got.from_main && clock > SWO_FRG_LIMIT_HZ) {
            if (first_above == 0U) {
                first_above = baud;
            }
        } else {
            assert(first_above == 0U);  // past the limit only from one rate up
        }
    }
    // swo_uart_clock's comment and README.md's "Past NXP's limits" quote this.
    assert(first_above > 20000000U && first_above <= 20200000U);
    printf("    the divider passes its limit from %u Hz\n", first_above);
}

int main(void) {
    test_written_fixed_points();
    test_written_never_long();
    test_unread_and_overrun();
    test_copy_wraps();
    test_reader_keeps_up_and_recovers(SWO_RING_BYTES);
    test_reader_keeps_up_and_recovers(CONSOLE_RING_BYTES);
    test_console_reader();
    test_divisors();
    test_uart_clock();
    return 0;
}
