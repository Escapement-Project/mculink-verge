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
    bool boundary;    // the real latency lies between `latency` and the next
    uint32_t edges;   // how a build whose range ends between them reads: edge_reads
    uint32_t tries;
    uint32_t host;    // the host's period
    uint64_t ran;     // latencies generated at it
    uint32_t repeats; // generations of one already there
};

/** Whether a build whose claimed range ends or starts between a boundary target's two latencies
 * reads right: 0 never, 1 always, 2 to 5 by its code, 6 to 9 afresh every generation, as a sample
 * on the edge of a bit does. */
static bool edge_reads(const struct target* t, uint32_t period, int32_t first, int32_t last) {
    if (t->edges < 2U) {
        return t->edges == 1U;
    }
    uint32_t h =
        (t->edges * 2246822519U) ^ (period * 2654435761U) ^ ((uint32_t)first * 40503U) ^ ((uint32_t)last * 97U);
    if (t->edges >= 6U) {
        h ^= t->tries * 3266489917U;
    }
    h ^= h >> 15;
    h *= 2246822519U;
    h ^= h >> 13;
    return (h & 1U) != 0U;
}

static struct jtag_gen_config config_for(uint32_t wire_, uint32_t period, uint32_t latency) {
    // A DAP next to TDO with one more TAP behind it, as on a CC13xx.
    return (struct jtag_gen_config){.wire = wire_, .period = period, .latency = latency, .before = 0U, .after = 1U};
}

static void try_latency(uint32_t period, uint32_t latency, struct latency_try* out, void* context) {
    struct target* t = context;
    struct jtag_gen_config config = config_for(t->wire, period, latency);
    config.earliest = 1U;
    jtag_gen_scan_fn scan;
    if (!jtag_gen_build(&config, &scan, nullptr, nullptr)) {
        return;
    }
    out->generated = true;
    out->first = jtag_gen_info.scan_first;
    out->last = jtag_gen_info.scan_last;
    out->same_below = jtag_gen_info.same_below;
    if (period == t->host) {
        const uint64_t bit = (uint64_t)1 << (latency - 1U);
        t->repeats += (t->ran & bit) != 0U ? 1U : 0U;
        t->ran |= bit;
    }
    // Sampled early, the reads agree on the bits before, and sampled late on the bits after: a
    // word with bit 0 set as often as not, and here always, so only the word says it is wrong.
    out->passed = t->latency != 0U;
    const int32_t n = (int32_t)t->latency;
    bool right = (out->first <= n) && (n <= out->last);
    if (t->boundary) {
        right = ((out->first <= n) && (n + 1 <= out->last)) ||
                (((out->last == n) || (out->first == n + 1)) && edge_reads(t, period, out->first, out->last));
    }
    out->word = right ? IDCODE : out->last < n ? (IDCODE << 1) | 1U : (IDCODE >> 1) | 0x80000000U;
    t->tries++;
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

/** The lowest latency every part of the last build reads right for. */
static int32_t lowest(void) {
    int32_t first = jtag_gen_info.scan_first;
    first = jtag_gen_info.read_first > first ? jtag_gen_info.read_first : first;
    return jtag_gen_info.write_first > first ? jtag_gen_info.write_first : first;
}

/** Whether the last build reads `latency` right everywhere, with `spare` cycles either side. */
static bool serves(int32_t latency, int32_t spare) { return (lowest() <= latency - spare) && (latency + spare <= fewest()); }

/** The safe read, the highest latency's build at the safe period, reads right for every latency
 * the search can find, on either wire. */
static void test_safe_period(void) {
    for (uint32_t w = 0U; w < 2U; w++) {
        struct jtag_gen_config config = config_for(w, JTAG_GEN_SAFE_PERIOD, LATENCY_MAX);
        config.earliest = 1U;
        jtag_gen_scan_fn scan;
        assert(jtag_gen_build(&config, &scan, nullptr, nullptr));
        assert(jtag_gen_info.scan_last >= (int32_t)LATENCY_MAX);
    }
}

/** What the search skips: a build that says every latency below a bound generates its code does.
 * Also that every build reads the latency it was built for with a cycle to spare either side, the
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
                    const bool built = scan_only ? jtag_gen_build(&config, &scan, nullptr, nullptr)
                                                 : jtag_gen_build(&config, &scan, &read, &write);
                    if (!built) {
                        continue;
                    }
                    const int32_t low_end = latency > 1U ? (int32_t)latency - 1 : 1;
                    assert((jtag_gen_info.scan_last > (int32_t)latency) && (jtag_gen_info.scan_first <= low_end));
                    if (!scan_only && jtag_gen_info.transfers) {
                        assert((fewest() > (int32_t)latency) && (lowest() <= low_end));
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

/** A stall changes only the builds that say it moves them, all JTAG, and a build with one still
 * reads the latency it was built for with a cycle to spare either side. cJTAG samples come before
 * their window's end. */
static void test_stall(void) {
    static const uint32_t periods[] = {3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U, 12U, 15U, 20U, 30U, 72U};
    uint32_t moved = 0U;
    uint32_t same = 0U;
    for (uint32_t w = 0U; w < JTAG_GEN_WIRES; w++) {
        for (uint32_t p = 0U; p < sizeof periods / sizeof periods[0]; p++) {
            for (uint32_t latency = 1U; latency <= LATENCY_MAX; latency++) {
                struct jtag_gen_config config = config_for(w, periods[p], latency);
                jtag_gen_scan_fn scan;
                jtag_gen_xfer_fn read;
                jtag_gen_xfer_fn write;
                if (!jtag_gen_build(&config, &scan, &read, &write)) {
                    continue;
                }
                const uint32_t calm = code_hash();
                const bool moves = jtag_gen_info.stall_moves;
                const bool transfers = jtag_gen_info.transfers;
                assert((w == JTAG_GEN_FOUR_WIRE) || !moves);
                config.stall = 2U;
                assert(jtag_gen_build(&config, &scan, &read, &write));
                assert(jtag_gen_info.transfers == transfers);
                const int32_t low_end = latency > 1U ? (int32_t)latency - 1 : 1;
                assert((jtag_gen_info.scan_last > (int32_t)latency) && (jtag_gen_info.scan_first <= low_end));
                assert(!transfers || ((fewest() > (int32_t)latency) && (lowest() <= low_end)));
                if (moves) {
                    moved++;
                } else {
                    assert(code_hash() == calm);
                    same++;
                }
            }
        }
    }
    printf("  stall: %u builds it may move, %u it leaves the same\n", moved, same);
}

/** A connection starts from the default latency and measures where the code built for it samples
 * less than two cycles past it (src/jtag_sweep.c, CONNECT_MARGIN). Every JTAG build a stall
 * may move is one of those, with the stall or without, so a capture starting or stopping never
 * changes code a connection read through without measuring. */
static void test_stall_connect(void) {
    uint32_t moving = 0U;
    for (uint32_t stall = 0U; stall <= 2U; stall += 2U) {
        for (uint32_t before = 0U; before <= 2U; before++) {
            for (uint32_t idle = 0U; idle <= 8U; idle += 4U) {
                for (uint32_t period = 3U; period <= 45U; period++) {
                    const struct jtag_gen_config config = {.wire = JTAG_GEN_FOUR_WIRE,
                                                           .period = period,
                                                           .latency = JTAG_GEN_FOUR_WIRE_LATENCY_DEFAULT,
                                                           .before = before,
                                                           .after = 1U,
                                                           .idle = idle,
                                                           .stall = stall};
                    jtag_gen_scan_fn scan;
                    jtag_gen_xfer_fn read;
                    jtag_gen_xfer_fn write;
                    if (!jtag_gen_build(&config, &scan, &read, &write) || !jtag_gen_info.stall_moves) {
                        continue;
                    }
                    moving++;
                    assert(fewest() - (int32_t)config.latency < 2);
                }
            }
        }
    }
    printf("  stall at connection: %u builds it may move, every one measured\n", moving);
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
    test_stall();
    test_stall_connect();

    static const uint32_t periods[] = {3U, 4U, 5U, 6U, 7U, 8U, 9U, 10U, 12U, 15U, 20U, 40U};
    static const char* const names[] = {"JTAG", "cJTAG"};
    uint32_t failures = 0U;
    for (uint32_t w = 0U; w < 2U; w++) {
        const uint32_t starts[] = {
            w == JTAG_GEN_TWO_PIN ? JTAG_GEN_LATENCY_DEFAULT : JTAG_GEN_FOUR_WIRE_LATENCY_DEFAULT, 1U, 30U};
        uint32_t outcomes[7] = {0};
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
                    const bool served = jtag_gen_build(&production, &scan, &read, &write) && serves((int32_t)latency, 0);
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
        // A latency on a cycle boundary, between n and n + 1: never a contradiction, both left a
        // cycle off, and what the search ends on serves both, whichever way the builds whose range
        // ends between them read. A boundary is found in one search that runs no build twice at
        // the host's period, leaves only latencies within a cycle of it, and ends on the latest
        // left, whose build has a cycle to spare: a later one costs clock rate.
        uint32_t on_boundary[7] = {0};
        uint32_t most_on_boundary = 0U;
        for (uint32_t p = 0U; p < sizeof periods / sizeof periods[0]; p++) {
            for (uint32_t n = 2U; n <= 39U; n++) {
                for (uint32_t s = 0U; s < sizeof starts / sizeof starts[0]; s++) {
                    for (uint32_t edges = 0U; edges <= 9U; edges++) {
                        const struct target fresh = {
                            .wire = w, .latency = n, .boundary = true, .edges = edges, .host = periods[p]};
                        struct target t = fresh;
                        struct latency_result r;
                        latency_measure(&wire, periods[p], starts[s], try_latency, &t, &r);
                        struct target again = fresh;
                        struct latency_result once;
                        latency_measure_once(&wire, periods[p], starts[s], try_latency, &again, &once);
                        on_boundary[r.outcome]++;
                        most_on_boundary = r.tries > most_on_boundary ? r.tries : most_on_boundary;
                        const uint32_t used = r.latency != 0U ? r.latency : starts[s];
                        const struct jtag_gen_config production = config_for(w, periods[p], used);
                        jtag_gen_scan_fn scan;
                        jtag_gen_xfer_fn read;
                        jtag_gen_xfer_fn write;
                        const bool built = jtag_gen_build(&production, &scan, &read, &write);
                        const bool served = built && serves((int32_t)n, 0) && serves((int32_t)n + 1, 0);
                        const bool boundary_right =
                            (r.outcome != LATENCY_BOUNDARY) ||
                            (built && (t.repeats == 0U) && ((int32_t)n + 2 <= fewest()) &&
                             (r.latency == latency_highest(r.tolerant)) &&
                             ((r.tolerant & ~latency_range((int32_t)n - 1, (int32_t)n + 2)) == 0U));
                        const uint64_t both = latency_range((int32_t)n, (int32_t)n + 1);
                        if ((r.outcome == LATENCY_INCONSISTENT) || (r.outcome == LATENCY_NO_BAND) ||
                            ((r.tolerant & both) != both) || (once.outcome != r.outcome) ||
                            (once.latency != r.latency) || (once.tries != r.tries) || (once.periods != r.periods) ||
                            !served || !boundary_right) {
                            printf("  %s period %u, between %u and %u, from %u, edges %u: outcome %u, latency %u, "
                                   "%u tries, left 0x%016llx, a cycle off 0x%016llx\n",
                                   names[w], periods[p], n, n + 1U, starts[s], edges, r.outcome, r.latency, r.tries,
                                   (unsigned long long)r.consistent, (unsigned long long)r.tolerant);
                            failures++;
                        }
                    }
                }
            }
        }

        // No answer: nothing passes, nothing changes, and one generation at the safe period says so.
        struct target silent = {w, 0U};
        struct latency_result r;
        latency_measure(&wire, 5U, starts[0], try_latency, &silent, &r);
        assert(r.outcome == LATENCY_NO_BAND && r.latency == 0U && r.tries == 1U);

        printf("  %s outcomes: %u measured, %u ambiguous, %u unresolved, %u no band, %u inconsistent, %u boundary\n",
               names[w], outcomes[1], outcomes[2], outcomes[3], outcomes[4], outcomes[5], outcomes[6]);
        printf("  %s between two latencies: %u measured, %u ambiguous, %u unresolved, %u boundary, at most %u "
               "generations\n",
               names[w], on_boundary[1], on_boundary[2], on_boundary[3], on_boundary[6], most_on_boundary);
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
