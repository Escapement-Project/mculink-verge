/** Measuring a target's read latency, free of hardware so the latency tests in tools/ can run it
 *  on the host.
 *
 * The latency is TIMING.md's A: the cycles from the store that makes the clock edge launching a
 * bit, SWCLK's rise or TCK's or TCKC's fall, to the first load that sees that bit. Code generated
 * for an assumed latency L reads right for a range of real ones, which each wire's generator
 * reports with each build. So every generation that runs reads says A is inside its range, if the
 * reads came back right, or outside it, if they did not. This keeps the latencies every result so
 * far agrees with, trying candidates outward from the one in use until both ends of the passing
 * band are found or one latency is left.
 *
 * A generator that takes the late side of the window as fixed reports ranges with no lower end: a
 * build reads right for every latency up to its range's end (latency_wire.open_below). There the
 * band only has a top, and the search walks one way from the latency in use: down while it reads
 * right, else up.
 *
 * At SWD's shortest read period a sample can only go on every other cycle, so two neighbouring
 * latencies read right with the same generations there. Where one period leaves them apart it
 * goes on at the next longer ones: reads at a slower clock than the host asked for, which no
 * target minds.
 *
 * It starts with one generation at the wire's safe period, or the host's if that is longer, built
 * for the highest latency, which reads right whatever the latency. A target that does not answer
 * there does not answer at all, and costs that one generation rather than one for every latency.
 * What it reads is the word the rest must match: a read whose samples come early or late can agree
 * on a wrong word, and on JTAG nothing else says so.
 *
 * A search that ends with no latency agreeing with every read runs again, up to LATENCY_ATTEMPTS
 * in all: a read another master stalled on the bit paths' port contradicts the rest (BUSES.md).
 */
#ifndef LATENCY_H
#define LATENCY_H

#include <stdbool.h>
#include <stdint.h>

/** The highest latency the generators take, and so the most a measurement can find. */
#define LATENCY_MAX 64U

/** Longer read periods tried after the host's, while two latencies or more are left. */
#define LATENCY_EXTRA_PERIODS 3U

/** Searches run while each ends with no latency agreeing with every read. */
#define LATENCY_ATTEMPTS 3U

/** What a measurement found. */
#define LATENCY_NEVER 0U        // none has run
#define LATENCY_MEASURED 1U     // one latency agrees with every read
#define LATENCY_AMBIGUOUS 2U    // two neighbouring latencies do: the later is used
#define LATENCY_UNRESOLVED 3U   // more do: the latest is used (see latency_measure)
#define LATENCY_NO_BAND 4U      // nothing read right at the host's period, or the safe read did not
#define LATENCY_INCONSISTENT 5U // no latency agrees with every read

/** What a wire's generator promises the search. */
struct latency_wire {
    // A read period at which the build for LATENCY_MAX reads right for every latency up to it.
    uint32_t safe_period;
    bool open_below; // every build reads right for every latency up to its range's end
};

/** What one generation's reads said. */
struct latency_try {
    bool generated; // false when the configuration could not be generated, and the rest is unset
    bool passed;    // every read came back OK and the same, a word the register could hold
    uint32_t word;  // what they read, when they passed
    int32_t first;  // the real latencies this generation reads right for
    int32_t last;
    // Every latency below this one generates the same code at this period.
    uint32_t same_below;
};

/** Generates for `latency` at read period `period` and reads DPIDR. */
typedef void (*latency_try_fn)(uint32_t period, uint32_t latency, struct latency_try* out, void* context);

struct latency_result {
    uint32_t outcome;    // LATENCY_*
    uint32_t latency;    // the one to use, or 0 to keep the latency in use
    uint64_t consistent; // bit n - 1 for every latency n every read agrees with
    uint32_t word;       // what the passing reads read
    uint32_t tries;      // generations that ran reads, every attempt's
    uint32_t periods;    // read periods measured at, the host's included, every attempt's
};

static inline uint64_t latency_range(int32_t first, int32_t last) {
    if (first < 1) {
        first = 1;
    }
    if (last > (int32_t)LATENCY_MAX) {
        last = (int32_t)LATENCY_MAX;
    }
    if (first > last) {
        return 0U;
    }
    const uint64_t to_last = (last == 64) ? ~(uint64_t)0 : (((uint64_t)1 << last) - 1U);
    return to_last & ~(((uint64_t)1 << (first - 1)) - 1U);
}

static inline uint32_t latency_count(uint64_t set) {
    uint32_t n = 0U;
    for (; set != 0U; set &= set - 1U) {
        n++;
    }
    return n;
}

static inline uint32_t latency_highest(uint64_t set) {
    uint32_t n = 0U;
    for (; set != 0U; set >>= 1) {
        n++;
    }
    return n;
}

struct latency_search {
    latency_try_fn try_latency;
    void* context;
    struct latency_result* result;
    bool have_word;    // result->word is set, by the safe read, which reads right whatever the latency
    bool start_passed; // the latency in use, at the host's period
    uint32_t period;
    uint64_t tried; // at this period
    uint64_t passed;
    uint32_t same_below; // at this period, once a generation has said, else 0
    bool same_tried;     // a latency below same_below has run
    bool same_passed;
};

/** Whether the search can stop: one latency or none left. */
static inline bool latency_done(const struct latency_search* s) { return latency_count(s->result->consistent) <= 1U; }

/** Tries `latency` at this period once, and keeps only the latencies its reads agree with. */
static inline bool latency_passes(struct latency_search* s, uint32_t latency) {
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
    struct latency_try t = {0};
    s->try_latency(s->period, latency, &t, s->context);
    if (!t.generated) {
        return false;
    }
    s->result->tries++;
    if (t.passed && s->have_word && (t.word != s->result->word)) {
        t.passed = false;
    }
    s->same_below = t.same_below;
    if (latency < t.same_below) {
        s->same_tried = true;
        s->same_passed = t.passed;
    }
    if (t.passed) {
        if (!s->have_word) {
            s->result->word = t.word;
            s->have_word = true;
        }
        s->passed |= bit;
    }
    const uint64_t served = latency_range(t.first, t.last);
    s->result->consistent &= t.passed ? served : ~served;
    return t.passed;
}

static inline void latency_new_period(struct latency_search* s, uint32_t period) {
    s->period = period;
    s->tried = 0U;
    s->passed = 0U;
    s->same_below = 0U;
    s->same_tried = false;
    s->result->periods++;
}

/** The passing band at one period, outward from `start`, until the search can stop. */
static inline void latency_band(struct latency_search* s, uint32_t start) {
    uint32_t found = 0U;
    for (uint32_t d = 0U; (found == 0U) && (d < LATENCY_MAX) && !latency_done(s); d++) {
        if ((start > d) && latency_passes(s, start - d)) {
            found = start - d;
        } else if ((start + d <= LATENCY_MAX) && !latency_done(s) && latency_passes(s, start + d)) {
            found = start + d;
        }
    }
    if (found == 0U) {
        return;
    }
    for (uint32_t l = found; (l > 1U) && !latency_done(s) && latency_passes(s, l - 1U); l--) {
    }
    for (uint32_t l = found; (l < LATENCY_MAX) && !latency_done(s) && latency_passes(s, l + 1U); l++) {
    }
}

/** The top of the passing band at one period, for a wire whose ranges are open below: down from
 * `start` while it reads right, else up until it does. */
static inline void latency_top(struct latency_search* s, uint32_t start) {
    if (latency_passes(s, start)) {
        for (uint32_t l = start; (l > 1U) && !latency_done(s) && latency_passes(s, l - 1U); l--) {
        }
    } else {
        for (uint32_t l = start + 1U; (l <= LATENCY_MAX) && !latency_done(s) && !latency_passes(s, l); l++) {
        }
    }
}

/** One search from `start`, the latency in use, at read period `period` and then longer ones. */
static inline void latency_measure_once(const struct latency_wire* wire, uint32_t period, uint32_t start,
                                        latency_try_fn try_latency, void* context, struct latency_result* result) {
    *result = (struct latency_result){.consistent = latency_range(1, (int32_t)LATENCY_MAX)};
    struct latency_search s = {.try_latency = try_latency, .context = context, .result = result};
    if ((start < 1U) || (start > LATENCY_MAX)) {
        start = 1U;
    }
    // The word the rest must match, from a build that reads it right whatever the latency.
    latency_new_period(&s, period > wire->safe_period ? period : wire->safe_period);
    bool answered = latency_passes(&s, LATENCY_MAX);
    for (uint32_t k = 0U; answered && (k <= LATENCY_EXTRA_PERIODS) && !latency_done(&s); k++) {
        latency_new_period(&s, period + k);
        if (wire->open_below) {
            latency_top(&s, start);
        } else {
            latency_band(&s, start);
        }
        if (k == 0U) {
            s.start_passed = (s.passed & ((uint64_t)1 << (start - 1U))) != 0U;
            // A slower period cannot say where the host's has to sample.
            answered = s.passed != 0U;
        }
    }

    const uint32_t left = latency_count(result->consistent);
    const uint32_t highest = latency_highest(result->consistent);
    if (!answered) {
        result->outcome = LATENCY_NO_BAND;
    } else if (left == 0U) {
        result->outcome = LATENCY_INCONSISTENT;
    } else if (left == 1U) {
        result->outcome = LATENCY_MEASURED;
        result->latency = highest;
    } else if ((left == 2U) && (latency_range((int32_t)highest - 1, (int32_t)highest) == result->consistent)) {
        // A bit that arrives later than assumed is the likelier mistake after a reset.
        result->outcome = LATENCY_AMBIGUOUS;
        result->latency = highest;
    } else {
        result->outcome = LATENCY_UNRESOLVED;
        // Where ranges have a lower end, the latency in use is kept if it read right. Where they do
        // not, a build reads right for every latency up to its range's end, so the latest left
        // serves them all.
        if (wire->open_below || !s.start_passed) {
            result->latency = highest;
        }
    }
}

/** Measures from `start`, the latency in use, at read period `period` and then longer ones. */
static inline void latency_measure(const struct latency_wire* wire, uint32_t period, uint32_t start,
                                   latency_try_fn try_latency, void* context, struct latency_result* result) {
    uint32_t tries = 0U;
    uint32_t periods = 0U;
    for (uint32_t attempt = 0U; attempt < LATENCY_ATTEMPTS; attempt++) {
        latency_measure_once(wire, period, start, try_latency, context, result);
        tries += result->tries;
        periods += result->periods;
        if (result->outcome != LATENCY_INCONSISTENT) {
            break;
        }
    }
    result->tries = tries;
    result->periods = periods;
}

#endif
