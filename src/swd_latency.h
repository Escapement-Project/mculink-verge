/** Measuring a target's SWD read latency, free of hardware so tools/swd_latency_test.c can run it
 *  on the host.
 *
 * The latency is TIMING.md's A: the cycles from the store that raises SWCLK to the first load that
 * sees the bit that edge launched. Code generated for an assumed latency L reads right for a range
 * of real ones, which src/swd_gen.c reports with each build (swd_gen_info.read_first and
 * read_last). So every generation that runs DPIDR reads says A is inside its range, if the reads
 * came back right, or outside it, if they did not. This keeps the latencies every result so far
 * agrees with, trying candidates outward from the one in use until both ends of the passing band
 * are found or one latency is left.
 *
 * At the shortest read period a sample can only go on every other cycle, so two neighbouring
 * latencies read right with the same generations there. Where one period leaves them apart it
 * goes on at the next longer ones: reads at a slower clock than the host asked for, which no
 * target minds.
 */
#ifndef SWD_LATENCY_H
#define SWD_LATENCY_H

#include <stdbool.h>
#include <stdint.h>

/** The highest latency the generator takes, and so the most a measurement can find. */
#define SWD_LATENCY_MAX 64U

/** Longer read periods tried after the host's, while two latencies or more are left. */
#define SWD_LATENCY_EXTRA_PERIODS 3U

/** What a measurement found. */
#define SWD_LATENCY_NEVER 0U        // none has run
#define SWD_LATENCY_MEASURED 1U     // one latency agrees with every read
#define SWD_LATENCY_AMBIGUOUS 2U    // two neighbouring latencies do: the later is used
#define SWD_LATENCY_UNRESOLVED 3U   // more do: the latency in use is kept if it read right, else the latest
#define SWD_LATENCY_NO_BAND 4U      // no generation read right
#define SWD_LATENCY_INCONSISTENT 5U // no latency agrees with every read, or two read different words

/** What one generation's reads said. */
struct swd_latency_try {
    bool generated; // false when the configuration could not be generated, and the rest is unset
    bool passed;    // every read came back the same, OK, with the right parity
    uint32_t word;  // what they read, when they passed
    int32_t first;  // the real latencies this generation reads right for
    int32_t last;
    // Every latency below this one generates the same code at this period.
    uint32_t same_below;
};

/** Generates for `latency` at read period `period` and reads DPIDR. */
typedef void (*swd_latency_try_fn)(uint32_t period, uint32_t latency, struct swd_latency_try* out, void* context);

struct swd_latency_result {
    uint32_t outcome;    // SWD_LATENCY_*
    uint32_t latency;    // the one to use, or 0 to keep the latency in use
    uint64_t consistent; // bit n - 1 for every latency n every read agrees with
    uint32_t word;       // what the passing reads read
    uint32_t tries;      // generations that ran reads
    uint32_t periods;    // read periods measured at, the host's included
};

static inline uint64_t swd_latency_range(int32_t first, int32_t last) {
    if (first < 1) {
        first = 1;
    }
    if (last > (int32_t)SWD_LATENCY_MAX) {
        last = (int32_t)SWD_LATENCY_MAX;
    }
    if (first > last) {
        return 0U;
    }
    const uint64_t to_last = (last == 64) ? ~(uint64_t)0 : (((uint64_t)1 << last) - 1U);
    return to_last & ~(((uint64_t)1 << (first - 1)) - 1U);
}

static inline uint32_t swd_latency_count(uint64_t set) {
    uint32_t n = 0U;
    for (; set != 0U; set &= set - 1U) {
        n++;
    }
    return n;
}

static inline uint32_t swd_latency_highest(uint64_t set) {
    uint32_t n = 0U;
    for (; set != 0U; set >>= 1) {
        n++;
    }
    return n;
}

struct swd_latency_search {
    swd_latency_try_fn try_latency;
    void* context;
    struct swd_latency_result* result;
    bool have_word;
    bool inconsistent;
    bool start_passed; // the latency in use, at the host's period
    uint32_t period;
    uint64_t tried; // at this period
    uint64_t passed;
    uint32_t same_below; // at this period, once a generation has said, else 0
    bool same_tried;     // a latency below same_below has run
    bool same_passed;
};

/** Whether the search can stop: one latency or none left, or two reads that disagree. */
static inline bool swd_latency_done(const struct swd_latency_search* s) {
    return s->inconsistent || (swd_latency_count(s->result->consistent) <= 1U);
}

/** Tries `latency` at this period once, and keeps only the latencies its reads agree with. */
static inline bool swd_latency_passes(struct swd_latency_search* s, uint32_t latency) {
    const uint64_t bit = (uint64_t)1 << (latency - 1U);
    if ((s->tried & bit) != 0U) {
        return (s->passed & bit) != 0U;
    }
    s->tried |= bit;
    // Code already run says nothing new.
    const bool same = latency < s->same_below;
    if (same && s->same_tried) {
        if (s->same_passed) {
            s->passed |= bit;
        }
        return s->same_passed;
    }
    struct swd_latency_try t = {0};
    s->try_latency(s->period, latency, &t, s->context);
    if (!t.generated) {
        return false;
    }
    s->result->tries++;
    s->same_below = t.same_below;
    if (latency < t.same_below) {
        s->same_tried = true;
        s->same_passed = t.passed;
    }
    if (t.passed) {
        if (!s->have_word) {
            s->result->word = t.word;
            s->have_word = true;
        } else if (t.word != s->result->word) {
            s->inconsistent = true;
            return false;
        }
        s->passed |= bit;
    }
    const uint64_t served = swd_latency_range(t.first, t.last);
    s->result->consistent &= t.passed ? served : ~served;
    return t.passed;
}

/** The passing band at one period, outward from `start`, until the search can stop. */
static inline void swd_latency_band(struct swd_latency_search* s, uint32_t start) {
    s->tried = 0U;
    s->passed = 0U;
    s->same_below = 0U;
    s->same_tried = false;
    uint32_t found = 0U;
    for (uint32_t d = 0U; (found == 0U) && (d < SWD_LATENCY_MAX) && !swd_latency_done(s); d++) {
        if ((start > d) && swd_latency_passes(s, start - d)) {
            found = start - d;
        } else if ((start + d <= SWD_LATENCY_MAX) && !swd_latency_done(s) && swd_latency_passes(s, start + d)) {
            found = start + d;
        }
    }
    if (found == 0U) {
        return;
    }
    for (uint32_t l = found; (l > 1U) && !swd_latency_done(s) && swd_latency_passes(s, l - 1U); l--) {
    }
    for (uint32_t l = found; (l < SWD_LATENCY_MAX) && !swd_latency_done(s) && swd_latency_passes(s, l + 1U); l++) {
    }
}

/** Measures from `start`, the latency in use, at read period `period` and then longer ones. */
static inline void swd_latency_measure(uint32_t period, uint32_t start, swd_latency_try_fn try_latency, void* context,
                                       struct swd_latency_result* result) {
    *result = (struct swd_latency_result){.consistent = swd_latency_range(1, (int32_t)SWD_LATENCY_MAX)};
    struct swd_latency_search s = {.try_latency = try_latency, .context = context, .result = result};
    if ((start < 1U) || (start > SWD_LATENCY_MAX)) {
        start = 1U;
    }
    for (uint32_t k = 0U; (k <= SWD_LATENCY_EXTRA_PERIODS) && !swd_latency_done(&s); k++) {
        s.period = period + k;
        result->periods++;
        swd_latency_band(&s, start);
        if (k == 0U) {
            s.start_passed = (s.passed & ((uint64_t)1 << (start - 1U))) != 0U;
        }
        if (!s.have_word) {
            // Nothing read right at any latency: no target answered.
            break;
        }
    }

    const uint32_t left = swd_latency_count(result->consistent);
    const uint32_t highest = swd_latency_highest(result->consistent);
    if (!s.have_word) {
        result->outcome = SWD_LATENCY_NO_BAND;
    } else if (s.inconsistent || (left == 0U)) {
        result->outcome = SWD_LATENCY_INCONSISTENT;
    } else if (left == 1U) {
        result->outcome = SWD_LATENCY_MEASURED;
        result->latency = highest;
    } else if ((left == 2U) && (swd_latency_range((int32_t)highest - 1, (int32_t)highest) == result->consistent)) {
        // A bit that arrives later than assumed is the likelier mistake after a reset.
        result->outcome = SWD_LATENCY_AMBIGUOUS;
        result->latency = highest;
    } else {
        result->outcome = SWD_LATENCY_UNRESOLVED;
        if (!s.start_passed) {
            result->latency = highest;
        }
    }
}

#endif
