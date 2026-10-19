/* src/latency.h's search against the real JTAG generator, both wires, with a target that answers by
 * latency. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>

#include "DAP.h"
#include "DAP_config.h"
#include "jtag_gen.h"
#include "latency.h"

DAP_Data_t DAP_Data;

static const struct latency_wire wire = {.safe_period = JTAG_GEN_SAFE_PERIOD, .open_below = true};

#define IDCODE 0x4BA00477U

struct target {
    uint32_t wire;
    uint32_t latency; // the real one, or 0 for a target that never answers
};

static struct jtag_gen_config config_for(uint32_t wire_, uint32_t period, uint32_t latency) {
    // A DAP next to TDO with one more TAP behind it, as on a CC13xx.
    return (struct jtag_gen_config){.wire = wire_, .period = period, .latency = latency, .before = 0U, .after = 1U};
}

static void try_latency(uint32_t period, uint32_t latency, struct latency_try* out, void* context) {
    const struct target* t = context;
    struct jtag_gen_config config = config_for(t->wire, period, latency);
    config.earliest = 1U;
    jtag_gen_scan_fn scan;
    if (!jtag_gen_build(&config, &scan, NULL, NULL)) {
        return;
    }
    out->generated = true;
    out->first = 1;
    out->last = jtag_gen_info.scan_last;
    out->same_below = jtag_gen_info.same_below;
    // Sampled early, the reads agree on the bits before: a word with bit 0 set as often as not.
    out->passed = t->latency != 0U;
    out->word = (int32_t)t->latency <= out->last ? IDCODE : (IDCODE << 1) | 1U;
}

static uint32_t code_hash(void) {
    uint32_t h = 2166136261U;
    for (uint32_t k = 0U; k < jtag_gen_info.code_bytes; k++) {
        h = (h ^ ((const uint8_t*)JTAG_GEN_BASE)[k]) * 16777619U;
    }
    return h;
}

static int32_t fewest(void) {
    int32_t last = jtag_gen_info.scan_last;
    last = jtag_gen_info.read_last < last ? jtag_gen_info.read_last : last;
    return jtag_gen_info.write_last < last ? jtag_gen_info.write_last : last;
}

/** The safe read, the highest latency's build at the safe period, reads right for every latency
 * the search can find, on either wire. */
static void test_safe_period(void) {
    for (uint32_t w = 0U; w < 2U; w++) {
        struct jtag_gen_config config = config_for(w, JTAG_GEN_SAFE_PERIOD, LATENCY_MAX);
        config.earliest = 1U;
        jtag_gen_scan_fn scan;
        assert(jtag_gen_build(&config, &scan, NULL, NULL));
        assert(jtag_gen_info.scan_last >= (int32_t)LATENCY_MAX);
    }
}

/** What the search skips: a build that says every latency below a bound generates its code does.
 * Also that every build samples no earlier than one cycle past the latency it was built for, the
 * full builds, the scans alone and the scans that sample as early as that allows. */
static void test_same_below(void) {
    static const uint32_t periods[] = {3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U, 12U, 15U, 20U, 30U, 72U, 150U};
    uint32_t claimed = 0U;
    for (uint32_t w = 0U; w < 2U; w++) {
        for (uint32_t p = 0U; p < sizeof periods / sizeof periods[0]; p++) {
            for (uint32_t scan_only = 0U; scan_only < 3U; scan_only++) {
                uint32_t hashes[LATENCY_MAX + 1U] = {0};
                uint32_t below[LATENCY_MAX + 1U] = {0};
                for (uint32_t latency = 1U; latency <= LATENCY_MAX; latency++) {
                    struct jtag_gen_config config = config_for(w, periods[p], latency);
                    config.earliest = scan_only == 2U ? 1U : 0U;
                    jtag_gen_scan_fn scan;
                    jtag_gen_xfer_fn read;
                    jtag_gen_xfer_fn write;
                    const bool built = scan_only ? jtag_gen_build(&config, &scan, NULL, NULL)
                                                 : jtag_gen_build(&config, &scan, &read, &write);
                    if (!built) {
                        continue;
                    }
                    assert(jtag_gen_info.scan_last > (int32_t)latency);
                    if (!scan_only && jtag_gen_info.transfers) {
                        assert(fewest() > (int32_t)latency);
                    }
                    hashes[latency] = code_hash();
                    below[latency] = jtag_gen_info.same_below;
                }
                for (uint32_t latency = 1U; latency <= LATENCY_MAX; latency++) {
                    for (uint32_t lower = 1U; lower < below[latency]; lower++) {
                        assert(hashes[lower] == hashes[latency]);
                        claimed++;
                    }
                }
            }
        }
    }
    printf("  same-code claims checked: %u\n", claimed);
}

int main(void) {
    if (mmap((void*)JTAG_GEN_BASE, 0x8000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
             -1, 0) != (void*)JTAG_GEN_BASE) {
        return 1;
    }
    if (mmap((void*)GPIO_ADDR, 0x4000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1,
             0) != (void*)GPIO_ADDR) {
        return 1;
    }
    test_safe_period();
    test_same_below();

    static const uint32_t periods[] = {3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U, 12U, 15U, 20U, 40U};
    static const char* const names[] = {"four-wire", "two-pin"};
    uint32_t failures = 0U;
    for (uint32_t w = 0U; w < 2U; w++) {
        const uint32_t starts[] = {
            w == JTAG_GEN_TWO_PIN ? JTAG_GEN_LATENCY_DEFAULT : JTAG_GEN_FOUR_WIRE_LATENCY_DEFAULT, 1U, 30U};
        uint32_t outcomes[6] = {0};
        uint32_t most_tries[sizeof periods / sizeof periods[0]] = {0};
        uint32_t near_default[sizeof periods / sizeof periods[0]] = {0};
        for (uint32_t p = 0U; p < sizeof periods / sizeof periods[0]; p++) {
            for (uint32_t latency = 2U; latency <= 40U; latency++) {
                for (uint32_t s = 0U; s < sizeof starts / sizeof starts[0]; s++) {
                    struct target t = {w, latency};
                    struct latency_result r;
                    latency_measure(&wire, periods[p], starts[s], try_latency, &t, &r);
                    outcomes[r.outcome]++;
                    most_tries[p] = r.tries > most_tries[p] ? r.tries : most_tries[p];
                    if ((s == 0U) && (latency >= 4U) && (latency <= 14U) && (r.tries > near_default[p])) {
                        near_default[p] = r.tries;
                    }
                    const uint32_t used = r.latency != 0U ? r.latency : starts[s];
                    const bool right = (r.outcome == LATENCY_MEASURED) ? (r.latency == latency)
                                       : (r.outcome == LATENCY_AMBIGUOUS)
                                           ? ((r.latency == latency) || (r.latency == latency + 1U))
                                           : (r.outcome == LATENCY_UNRESOLVED);
                    // Wherever the probe can read this target at all, what it ends on reads it, scans and
                    // transfers alike.
                    const struct jtag_gen_config production = config_for(w, periods[p], used);
                    jtag_gen_scan_fn scan;
                    jtag_gen_xfer_fn read;
                    jtag_gen_xfer_fn write;
                    const bool served =
                        jtag_gen_build(&production, &scan, &read, &write) && ((int32_t)latency <= fewest());
                    if (!right || !served || (r.word != IDCODE)) {
                        printf("  %s period %u, latency %u, from %u: outcome %u, latency %u, %u tries, "
                               "left 0x%016llx\n",
                               names[w], periods[p], latency, starts[s], r.outcome, r.latency, r.tries,
                               (unsigned long long)r.consistent);
                        failures++;
                    }
                }
            }
        }
        // No answer: nothing passes, nothing changes, and one generation at the safe period says so.
        struct target silent = {w, 0U};
        struct latency_result r;
        latency_measure(&wire, 5U, starts[0], try_latency, &silent, &r);
        assert(r.outcome == LATENCY_NO_BAND && r.latency == 0U && r.tries == 1U);

        printf("  %s outcomes: %u measured, %u ambiguous, %u unresolved, %u no band, %u inconsistent\n", names[w],
               outcomes[1], outcomes[2], outcomes[3], outcomes[4], outcomes[5]);
        printf("  most generations, by period:");
        for (uint32_t p = 0U; p < sizeof periods / sizeof periods[0]; p++) {
            printf(" %u:%u", periods[p], most_tries[p]);
        }
        printf("\n  from the default to a latency of 4 to 14:");
        for (uint32_t p = 0U; p < sizeof periods / sizeof periods[0]; p++) {
            printf(" %u:%u", periods[p], near_default[p]);
        }
        printf("\n");
    }
    return failures != 0U;
}
