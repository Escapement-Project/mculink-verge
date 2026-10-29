/* src/latency.h's search against the real generator, with a target that answers by latency. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>

#include "DAP.h"
#include "DAP_config.h"
#include "latency.h"
#include "swd_connect.h"
#include "swd_gen.h"

DAP_Data_t DAP_Data;

static const struct latency_wire wire = {.safe_period = SWD_GEN_SAFE_PERIOD, .open_below = false};

#define DPIDR 0x6BA02477U

struct target {
    uint32_t latency; // the real one, or 0 for a target that never answers
    uint32_t write_period;
    uint32_t read_period;
    uint32_t odd_word_at; // a latency whose reads come back right but different, or 0
    uint32_t odd_tries;   // and only for this many generations, or 0 for all of them
    uint32_t tries;
};

static bool served(const struct swd_gen_config* config, uint32_t latency) {
    swd_gen_fn read;
    swd_gen_fn write;
    if (!swd_gen_build(config, &read, &write)) {
        return false;
    }
    return (swd_gen_info.read_first <= (int32_t)latency) && ((int32_t)latency <= swd_gen_info.read_last) &&
           (swd_gen_info.write_first <= (int32_t)latency) && ((int32_t)latency <= swd_gen_info.write_last);
}

static void try_latency(uint32_t period, uint32_t latency, struct latency_try* out, void* context) {
    struct target* t = context;
    const struct swd_gen_config config = {
        .period = period == t->read_period ? t->write_period : period,
        .latency = latency,
        .turnaround = 1U,
        .data_phase = 1U,
    };
    swd_gen_fn read;
    swd_gen_fn write;
    if (!swd_gen_build(&config, &read, &write)) {
        return;
    }
    out->generated = true;
    out->first = swd_gen_info.read_first;
    out->last = swd_gen_info.read_last;
    out->same_below = swd_gen_info.masked_from;
    out->passed = (t->latency != 0U) && (out->first <= (int32_t)t->latency) && ((int32_t)t->latency <= out->last);
    const bool odd = (latency == t->odd_word_at) && ((t->odd_tries == 0U) || (t->tries < t->odd_tries));
    out->word = odd ? DPIDR ^ 0x10U : DPIDR;
    t->tries++;
}

static uint32_t code_hash(void) {
    uint32_t h = 2166136261U;
    for (uint32_t k = 0U; k < swd_gen_info.code_bytes; k++) {
        h = (h ^ ((const uint8_t*)SWD_GEN_BASE)[k]) * 16777619U;
    }
    return h;
}

/** The safe period reads right for every latency the search can find, whatever it is built for. */
static void test_safe_period(void) {
    for (uint32_t turnaround = 1U; turnaround <= 4U; turnaround++) {
        for (uint32_t latency = 1U; latency <= LATENCY_MAX; latency++) {
            const struct swd_gen_config config = {
                .period = SWD_GEN_SAFE_PERIOD, .latency = latency, .turnaround = turnaround, .data_phase = 1U};
            swd_gen_fn read;
            swd_gen_fn write;
            assert(swd_gen_build(&config, &read, &write));
            assert(swd_gen_info.read_first <= 1 && swd_gen_info.read_last >= (int32_t)LATENCY_MAX);
        }
    }
}

/** What the search skips: below masked_from, every latency generates the same code. */
static void test_same_below(void) {
    for (uint32_t period = 4U; period <= 160U; period += period < 24U ? 1U : 17U) {
        uint32_t first = 0U;
        for (uint32_t latency = 1U; latency <= LATENCY_MAX; latency++) {
            const struct swd_gen_config config = {.period = period, .latency = latency, .turnaround = 1U};
            swd_gen_fn read;
            swd_gen_fn write;
            assert(swd_gen_build(&config, &read, &write));
            if (latency >= swd_gen_info.masked_from) {
                break;
            }
            const uint32_t h = code_hash();
            if (latency == 1U) {
                first = h;
            }
            assert(h == first);
        }
    }
}

/** The fewest cycles `latency` has from either end of what the last build serves. */
static int32_t served_margin(uint32_t latency) {
    const int32_t l = (int32_t)latency;
    const int32_t sides[] = {l - swd_gen_info.read_first, swd_gen_info.read_last - l, l - swd_gen_info.write_first,
                             swd_gen_info.write_last - l};
    int32_t fewest = sides[0];
    for (uint32_t i = 1U; i < sizeof sides / sizeof sides[0]; i++) {
        fewest = sides[i] < fewest ? sides[i] : fewest;
    }
    return fewest;
}

/** With a stall the latency built for stays a cycle inside what the build serves once a sample is
 * held, wherever it does without one, and reads run no faster than the stall's floor. */
static uint32_t test_stall(void) {
    uint32_t failures = 0U;
    for (uint32_t period = 3U; period <= 24U; period++) {
        for (uint32_t latency = 1U; latency <= 40U; latency++) {
            for (uint32_t turnaround = 1U; turnaround <= 4U; turnaround++) {
                struct swd_gen_config config = {
                    .period = period, .latency = latency, .turnaround = turnaround, .data_phase = 1U};
                swd_gen_fn read;
                swd_gen_fn write;
                assert(swd_gen_build(&config, &read, &write));
                const int32_t calm = served_margin(latency);
                config.stall = 2U;
                assert(swd_gen_build(&config, &read, &write));
                assert(swd_gen_info.read_period >= SWD_GEN_STALL_READ_PERIOD_MIN(2U));
                const int32_t held = served_margin(latency);
                if ((calm >= 1) && (held < 1)) {
                    printf("  period %u, latency %u, turnaround %u: margin %d without a stall, %d with\n", period,
                           latency, turnaround, calm, held);
                    failures++;
                }
            }
        }
    }
    return failures;
}

/** `count` bits of `data`, fed in pieces of `piece` bits as separate sequences would be. */
static void feed(struct swd_connect* c, uint64_t data, uint32_t count, uint32_t piece) {
    for (uint32_t at = 0U; at < count; at += piece) {
        const uint32_t n = count - at < piece ? count - at : piece;
        uint8_t bytes[8];
        const uint64_t chunk = data >> at;
        for (uint32_t i = 0U; i < 8U; i++) {
            bytes[i] = (uint8_t)(chunk >> (8U * i));
        }
        swd_connect_sequence(c, bytes, n);
    }
}

/** probe-rs's swd_line_reset: 51 clocks high, then `low` low, in one sequence. */
static void line_reset(struct swd_connect* c, uint32_t low, uint32_t piece) {
    feed(c, ((uint64_t)1 << 51) - 1U, 51U + low, piece);
}

/** probe-rs's TARGETSEL write, from debug_port_connect. */
static void targetsel(struct swd_connect* c, uint32_t value, bool bad_parity, uint32_t piece) {
    const uint64_t parity = (uint64_t)((uint32_t)__builtin_parity(value) ^ (bad_parity ? 1U : 0U));
    feed(c, (parity << 45) | ((uint64_t)value << 13) | 0x1F99U, 48U, piece);
}

static void test_connect(void) {
    static const uint32_t pieces[] = {1U, 3U, 8U, 64U};
    for (uint32_t i = 0U; i < sizeof pieces / sizeof pieces[0]; i++) {
        const uint32_t piece = pieces[i];
        struct swd_connect c = {0};
        line_reset(&c, 3U, piece);
        assert(c.reset && !c.multidrop);

        c.reset = false;
        line_reset(&c, 3U, piece);
        targetsel(&c, 0x01002927U, false, piece);
        assert(c.reset && c.multidrop && c.targetsel == 0x01002927U);

        // A second line reset forgets the TARGETSEL before it.
        line_reset(&c, 2U, piece);
        assert(c.reset && !c.multidrop);
        targetsel(&c, 0x11002927U, false, piece);
        assert(c.multidrop && c.targetsel == 0x11002927U);

        struct swd_connect bad = {0};
        line_reset(&bad, 3U, piece);
        targetsel(&bad, 0x01002927U, true, piece);
        assert(bad.reset && !bad.multidrop);

        // 49 high clocks are not a line reset.
        struct swd_connect short_reset = {0};
        feed(&short_reset, ((uint64_t)1 << 49) - 1U, 52U, piece);
        assert(!short_reset.reset);

        // What the probe sends when it selects the target again: probe-rs's own form.
        assert(swd_connect_targetsel(0x01002927U) ==
               (((uint64_t)__builtin_parity(0x01002927U) << 45) | ((uint64_t)0x01002927U << 13) | 0x1F99U));
        struct swd_connect again = {0};
        line_reset(&again, 3U, piece);
        feed(&again, swd_connect_targetsel(0x01002927U), SWD_CONNECT_TARGETSEL_CLOCKS, piece);
        assert(again.multidrop && again.targetsel == 0x01002927U);

        // JTAG to SWD: a line reset, the select word, a line reset, idle.
        struct swd_connect jtag = {0};
        feed(&jtag, ((uint64_t)1 << 51) - 1U, 51U, piece);
        feed(&jtag, 0xE79EU, 16U, piece);
        line_reset(&jtag, 3U, piece);
        assert(jtag.reset && !jtag.multidrop);
    }
}

static void test_range(void) {
    assert(latency_range(1, 64) == ~(uint64_t)0);
    assert(latency_range(-3, 1) == 1U);
    assert(latency_range(64, 70) == (uint64_t)1 << 63);
    assert(latency_range(5, 4) == 0U);
    assert(latency_range(3, 4) == 0xCU);
    assert(latency_count(0xF0F0U) == 8U);
    assert(latency_highest(0x30U) == 6U);
    assert(latency_highest(0U) == 0U);
}

int main(void) {
    if (mmap((void*)SWD_GEN_BASE, 0x8000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1,
             0) != (void*)SWD_GEN_BASE) {
        return 1;
    }
    if (mmap((void*)GPIO_ADDR, 0x4000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1,
             0) != (void*)GPIO_ADDR) {
        return 1;
    }
    test_range();
    test_same_below();
    test_safe_period();
    const uint32_t stall_failures = test_stall();
    test_connect();

    static const uint32_t periods[][2] = {{3, 4},   {4, 4},   {5, 5},   {6, 6},   {7, 7},   {8, 8},  {9, 9},
                                          {10, 10}, {12, 12}, {13, 13}, {16, 16}, {20, 20}, {40, 40}};
    static const uint32_t starts[] = {7U, 1U, 30U};
    uint32_t outcomes[6] = {0};
    uint32_t most_tries[sizeof periods / sizeof periods[0]] = {0};
    uint32_t failures = stall_failures;
    for (uint32_t p = 0U; p < sizeof periods / sizeof periods[0]; p++) {
        for (uint32_t latency = 2U; latency <= 40U; latency++) {
            for (uint32_t s = 0U; s < sizeof starts / sizeof starts[0]; s++) {
                struct target t = {latency, periods[p][0], periods[p][1], 0U};
                struct latency_result r;
                latency_measure(&wire, t.read_period, starts[s], try_latency, &t, &r);
                outcomes[r.outcome]++;
                // Only a search that ends inconsistent runs again.
                struct latency_result once;
                latency_measure_once(&wire, t.read_period, starts[s], try_latency, &t, &once);
                if ((r.outcome != LATENCY_INCONSISTENT) &&
                    ((once.outcome != r.outcome) || (once.latency != r.latency) || (once.tries != r.tries) ||
                     (once.periods != r.periods))) {
                    printf("  period %u/%u, latency %u, from %u: searched again after outcome %u\n", t.write_period,
                           t.read_period, latency, starts[s], r.outcome);
                    failures++;
                }
                most_tries[p] = r.tries > most_tries[p] ? r.tries : most_tries[p];
                const uint32_t used = r.latency != 0U ? r.latency : starts[s];
                const struct swd_gen_config production = {
                    .period = t.write_period, .latency = used, .turnaround = 1U, .data_phase = 0U};
                // Wherever the probe can read this target at all, what it ends on reads it.
                const bool right = (r.outcome == LATENCY_MEASURED) ? (r.latency == latency)
                                   : (r.outcome == LATENCY_AMBIGUOUS)
                                       ? ((r.latency == latency) || (r.latency == latency + 1U))
                                       : (r.outcome == LATENCY_UNRESOLVED);
                if (!right || !served(&production, latency) || (r.word != DPIDR)) {
                    printf("  period %u/%u, latency %u, from %u: outcome %u, latency %u, %u tries, "
                           "left 0x%016llx\n",
                           t.write_period, t.read_period, latency, starts[s], r.outcome, r.latency, r.tries,
                           (unsigned long long)r.consistent);
                    failures++;
                }
            }
        }
    }

    // No answer: nothing passes, nothing changes, and one generation at the safe period says so.
    struct target silent = {0U, 4U, 4U, 0U};
    struct latency_result r;
    latency_measure(&wire, 4U, 7U, try_latency, &silent, &r);
    assert(r.outcome == LATENCY_NO_BAND && r.latency == 0U && r.tries == 1U);

    // A word other than the safe read's is a read gone wrong, however the reads agree. This target
    // reads it at a latency whose range holds its own, so no latency agrees with every read.
    struct target odd = {7U, 4U, 4U, 6U};
    latency_measure(&wire, 4U, 7U, try_latency, &odd, &r);
    assert((r.word == DPIDR) && (r.outcome == LATENCY_INCONSISTENT) && (r.latency == 0U));
    assert((odd.tries == r.tries) && (r.tries % LATENCY_ATTEMPTS == 0U) && (r.periods % LATENCY_ATTEMPTS == 0U));
    const uint32_t first_periods = r.periods / LATENCY_ATTEMPTS;

    // The same read gone wrong only during the first search, as a stall by another master on the
    // port does: the next search ends where an undisturbed one does, and the generations add up.
    const uint32_t first_search = odd.tries / LATENCY_ATTEMPTS;
    struct target clean = {7U, 4U, 4U, 0U};
    struct latency_result undisturbed;
    latency_measure(&wire, 4U, 7U, try_latency, &clean, &undisturbed);
    struct target disturbed = {7U, 4U, 4U, 6U, first_search};
    latency_measure(&wire, 4U, 7U, try_latency, &disturbed, &r);
    assert((r.outcome == undisturbed.outcome) && (r.latency == undisturbed.latency) && (r.latency != 0U) &&
           (r.tries == first_search + undisturbed.tries) && (disturbed.tries == r.tries) &&
           (r.periods == first_periods + undisturbed.periods));

    printf("  outcomes: %u measured, %u ambiguous, %u unresolved, %u no band, %u inconsistent\n", outcomes[1],
           outcomes[2], outcomes[3], outcomes[4], outcomes[5]);
    printf("  most generations, by read period:");
    for (uint32_t p = 0U; p < sizeof periods / sizeof periods[0]; p++) {
        printf(" %u/%u:%u", periods[p][0], periods[p][1], most_tries[p]);
    }
    printf("\n");
    return failures != 0U;
}
