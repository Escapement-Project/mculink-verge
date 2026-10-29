/** Verge Aux's energy stream (HOST-INTERFACE.md, "The energy stream"): the messages, the timeline
 *  that gives each pair its time, and the ring CPU 0 sends from. Free of hardware, so
 *  tools/energy_stream_test.c runs it on the host.
 *
 * The core that owns the capture state under the quiet handshake (src/energy_integration.h) writes
 * all of it, with two exceptions. CPU 1 records holds (`holds_written`) and a start's first trigger
 * (`timeline_pending`) while CPU 0 may own the rest. CPU 0's loop reads published messages and
 * moves `sent`.
 *
 * Times are counts of the clock the conversion timer runs from. The timer that stamps them is 32
 * bits and wraps in under a minute at that clock, so every comparison here is between nearby
 * values: the next pair's trigger against a hold that has just happened, or a resume against the
 * pair before its pause.
 */
#ifndef ENERGY_STREAM_H
#define ENERGY_STREAM_H

#include <assert.h>
#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "energy_samples.h"

#define ENERGY_STREAM_SLOTS 16U
#define ENERGY_STREAM_MESSAGE_MAX 512U
#define ENERGY_STREAM_PAIRS_MAX 99U
#define ENERGY_STREAM_PAIR_BYTES 5U
#define ENERGY_STREAM_PAIRS_FIXED 16U
#define ENERGY_STREAM_HEADER_BYTES 68U
#define ENERGY_STREAM_END_BYTES 6U
#define ENERGY_STREAM_HOLDS 8U
/** Pairs messages leave this many slots for the header and end messages a stream needs. */
#define ENERGY_STREAM_CONTROL_SLOTS 2U
/** How long a start may take to publish its first trigger once its pairs can arrive. */
#define ENERGY_STREAM_PENDING_POLLS 100000U

static_assert((ENERGY_STREAM_SLOTS & (ENERGY_STREAM_SLOTS - 1U)) == 0U, "slots index by a mask");
static_assert(ENERGY_STREAM_PAIRS_FIXED + ENERGY_STREAM_PAIRS_MAX * ENERGY_STREAM_PAIR_BYTES < ENERGY_STREAM_MESSAGE_MAX,
              "a pairs message fits a packet with no zero-length packet after it");

/** CPU 0 runs these from SRAM, in its DMA interrupt and its loop; a copy a compiler kept out of
 *  line would land in flash. */
#define ENERGY_STREAM_INLINE static inline __attribute__((always_inline))

enum : uint8_t {
    ENERGY_STREAM_PAIRS = 0xA0U,
    ENERGY_STREAM_HEADER = 0xA1U,
    ENERGY_STREAM_END = 0xA2U,
    ENERGY_STREAM_EDGES = 0xA3U,
};

/** The end message's reason. */
enum : uint8_t {
    ENERGY_STREAM_END_ASKED = 1U,
    ENERGY_STREAM_END_STOPPED = 2U,
    ENERGY_STREAM_END_RESTARTED = 3U,
    ENERGY_STREAM_END_FAULT = 4U,
};

/** A pairs message's flags: the capture paused before its first pair, which ends a pair waiting
 *  for the next one's current (src/energy_pairs.h); or a read-latency hold stopped its conversions,
 *  which moves time and nothing else. */
#define ENERGY_STREAM_FLAG_PAUSED 1U
#define ENERGY_STREAM_FLAG_HELD 2U

/** A read-latency hold, as stamping-timer values of the first trigger due after the conversion
 *  timer stopped: when it was due, give or take the few counts the stop's stamp lags, and when the
 *  restarted timer brings it. */
typedef struct {
    uint32_t due;
    uint32_t came;
} energy_stream_hold_t;

struct energy_stream {
    /** The next pair to time: its sequence, its trigger as the stamping timer reads it, and its
     *  trigger in counts since the capture started. */
    uint32_t next_sequence;
    uint32_t next_raw;
    uint64_t next_counts;
    uint32_t period;
    uint32_t clock_hz;
    /** Set by a start or a resume while it owns the state, cleared once its timer runs and `next_raw`
     *  holds the first trigger. No pair can complete before then. */
    volatile bool timeline_pending;
    volatile uint32_t holds_written;
    volatile uint32_t holds_read;
    /** `holds_written` as the batch of pairs being integrated began. CPU 1 records a hold before the
     *  timer runs again, so every hold a batch's pairs come after is recorded by then. */
    uint32_t holds_seen;
    energy_stream_hold_t holds[ENERGY_STREAM_HOLDS];

    bool active;
    /** ENERGY_STREAM_FLAG_* since the last pairs message: the next one starts afresh and says so. */
    uint8_t gap;
    /** The pairs message being built: its pair count (0 when none) and its first pair's time. A
     *  message that found the ring full keeps counting but goes nowhere. */
    uint16_t open_pairs;
    bool open_dropped;
    uint64_t open_counts;

    volatile uint32_t written;
    volatile uint32_t sent;
    uint16_t length[ENERGY_STREAM_SLOTS];
    alignas(4) uint8_t slot[ENERGY_STREAM_SLOTS][ENERGY_STREAM_MESSAGE_MAX];
};

/** What a stream's header carries besides its tables (HOST-INTERFACE.md, "Header: 0xa1"). */
typedef struct {
    uint32_t rate_hz;
    uint8_t setting; // 1 for 350 mA, 2 for 50 mA, 0 unknown
    uint16_t full_scale_mv;
    uint32_t first_sequence;
    bool previous_low;
    energy_calibration_t const* low;
    energy_calibration_t const* high;
    uint8_t dio_mask;
    uint8_t dio_levels;
    uint16_t edge_resolution_ns;
} energy_stream_header_t;

ENERGY_STREAM_INLINE void energy_stream_barrier(void) {
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

ENERGY_STREAM_INLINE uint8_t* energy_stream_u16(uint8_t* out, uint16_t value) {
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8U);
    return out + 2;
}

ENERGY_STREAM_INLINE uint8_t* energy_stream_u32(uint8_t* out, uint32_t value) {
    for (uint32_t i = 0U; i < 4U; i++) {
        out[i] = (uint8_t)(value >> (8U * i));
    }
    return out + 4;
}

ENERGY_STREAM_INLINE uint8_t* energy_stream_u64(uint8_t* out, uint64_t value) {
    for (uint32_t i = 0U; i < 8U; i++) {
        out[i] = (uint8_t)(value >> (8U * i));
    }
    return out + 8;
}

/** `n / d` and its remainder, for a quotient under 2^32. The C library's 64-bit division runs from
 *  flash, and this runs in CPU 0's DMA interrupt, from SRAM; constant shifts keep it out of the
 *  library too. */
ENERGY_STREAM_INLINE uint32_t energy_stream_divide(uint64_t n, uint32_t d, uint32_t* remainder) {
    uint64_t r = n >> 32U;
    uint32_t low = (uint32_t)n;
    uint32_t q = 0U;
    for (uint32_t i = 0U; i < 32U; i++) {
        r = (r << 1U) | (low >> 31U);
        low <<= 1U;
        q <<= 1U;
        if (r >= d) {
            r -= d;
            q |= 1U;
        }
    }
    *remainder = (uint32_t)r;
    return q;
}

/** Counts to nanoseconds, rounded down: whole seconds, then the counts left over. Exact for 2^32
 *  seconds. */
ENERGY_STREAM_INLINE uint64_t energy_stream_ns(uint64_t counts, uint32_t clock_hz) {
    uint32_t left = 0U;
    uint32_t const seconds = energy_stream_divide(counts, clock_hz, &left);
    uint32_t unused = 0U;
    return (uint64_t)seconds * 1000000000U + energy_stream_divide((uint64_t)left * 1000000000U, clock_hz, &unused);
}

ENERGY_STREAM_INLINE uint32_t energy_stream_free(struct energy_stream const* stream) {
    return ENERGY_STREAM_SLOTS - (stream->written - stream->sent);
}

ENERGY_STREAM_INLINE uint8_t* energy_stream_slot(struct energy_stream* stream) {
    return stream->slot[stream->written & (ENERGY_STREAM_SLOTS - 1U)];
}

/** Hand the slot at `written` to CPU 0's loop. */
ENERGY_STREAM_INLINE void energy_stream_publish(struct energy_stream* stream, uint16_t length) {
    stream->length[stream->written & (ENERGY_STREAM_SLOTS - 1U)] = length;
    energy_stream_barrier();
    stream->written++;
}

ENERGY_STREAM_INLINE void energy_stream_close(struct energy_stream* stream) {
    if ((stream->open_pairs != 0U) && !stream->open_dropped) {
        uint8_t* const message = energy_stream_slot(stream);
        (void)energy_stream_u16(message + 14, stream->open_pairs);
        energy_stream_publish(stream,
                              (uint16_t)(ENERGY_STREAM_PAIRS_FIXED + stream->open_pairs * ENERGY_STREAM_PAIR_BYTES));
    }
    stream->open_pairs = 0U;
    stream->open_dropped = false;
}

/** The first message of a stream. Starts the stream only if the ring had room for it. */
ENERGY_STREAM_INLINE bool energy_stream_begin(struct energy_stream* stream, energy_stream_header_t const* header) {
    stream->open_pairs = 0U;
    stream->open_dropped = false;
    stream->gap = 0U;
    if (energy_stream_free(stream) == 0U) {
        stream->active = false;
        return false;
    }
    uint8_t* out = energy_stream_slot(stream);
    *out++ = ENERGY_STREAM_HEADER;
    *out++ = 1U;
    out = energy_stream_u32(out, header->rate_hz);
    *out++ = header->setting;
    out = energy_stream_u16(out, header->full_scale_mv);
    out = energy_stream_u32(out, header->first_sequence);
    *out++ = header->previous_low ? 1U : 0U;
    energy_calibration_t const* const tables[] = {header->low, header->high};
    for (uint32_t t = 0U; t < 2U; t++) {
        uint8_t const count = tables[t]->count < 4U ? tables[t]->count : 4U;
        *out++ = count;
        for (uint32_t i = 0U; i < 4U; i++) {
            out = energy_stream_u16(out, i < count ? tables[t]->code[i] : 0U);
            out = energy_stream_u32(out, i < count ? tables[t]->current_na[i] : 0U);
        }
    }
    *out++ = header->dio_mask;
    *out++ = header->dio_levels;
    (void)energy_stream_u16(out, header->edge_resolution_ns);
    energy_stream_publish(stream, ENERGY_STREAM_HEADER_BYTES);
    stream->active = true;
    return true;
}

/** The last message of a stream, after the pairs it was building. */
ENERGY_STREAM_INLINE void energy_stream_end(struct energy_stream* stream, uint8_t reason, uint32_t last_sequence) {
    if (!stream->active) {
        return;
    }
    energy_stream_close(stream);
    stream->active = false;
    if (energy_stream_free(stream) == 0U) {
        return;
    }
    uint8_t* const out = energy_stream_slot(stream);
    out[0] = ENERGY_STREAM_END;
    out[1] = reason;
    (void)energy_stream_u32(out + 2, last_sequence);
    energy_stream_publish(stream, ENERGY_STREAM_END_BYTES);
}

/** A pause: the pairs so far leave, and the next message says the capture paused. */
ENERGY_STREAM_INLINE void energy_stream_pause(struct energy_stream* stream) {
    energy_stream_close(stream);
    stream->gap |= ENERGY_STREAM_FLAG_PAUSED;
}

/** A start (`first_sequence` 1) or a resume, while its caller owns the state: no pair until
 *  energy_stream_timeline_run. A resume's pending holds all came before its pause. */
static inline void energy_stream_timeline_prepare(struct energy_stream* stream, uint32_t first_sequence) {
    stream->timeline_pending = true;
    stream->next_sequence = first_sequence;
    stream->holds_read = stream->holds_written;
    stream->holds_seen = stream->holds_read;
}

/** Once the conversion timer runs: its first trigger, `first_raw`, comes `ticks` after the stamp
 *  taken as it started. A start's counts begin at that stamp; a resume continues from the pair
 *  before its pause, `next_counts` and `next_raw` as it left them, by however long the pause took. */
static inline void energy_stream_timeline_run(struct energy_stream* stream, bool new_session, uint32_t clock_hz,
                                              uint32_t ticks, uint32_t started_raw) {
    uint32_t const first_raw = started_raw + ticks;
    if (new_session) {
        stream->next_counts = ticks;
    } else {
        stream->next_counts = (uint64_t)((int64_t)stream->next_counts + (int32_t)(first_raw - stream->next_raw));
    }
    stream->next_raw = first_raw;
    stream->period = 2U * ticks;
    stream->clock_hz = clock_hz;
    energy_stream_barrier();
    stream->timeline_pending = false;
}

/** CPU 1, as a read-latency hold ends: records when its first trigger was due, and hands back the
 *  record, or NULL with the ring full, for `came`. No pair the hold delayed can arrive before the
 *  timer runs again, so `came` is final if written before that. */
static inline energy_stream_hold_t* energy_stream_hold(struct energy_stream* stream, uint32_t due) {
    uint32_t const at = stream->holds_written;
    if ((at - stream->holds_read) >= ENERGY_STREAM_HOLDS) {
        return NULL;
    }
    energy_stream_hold_t* const hold = &stream->holds[at % ENERGY_STREAM_HOLDS];
    *hold = (energy_stream_hold_t){due, due};
    energy_stream_barrier();
    stream->holds_written = at + 1U;
    return hold;
}

/** Before the owner integrates a batch of pairs: a start's timeline, and the holds recorded so far. */
ENERGY_STREAM_INLINE void energy_stream_batch_begin(struct energy_stream* stream) {
    for (uint32_t polls = 0U; stream->timeline_pending && (polls < ENERGY_STREAM_PENDING_POLLS); polls++) {
    }
    energy_stream_barrier();
    stream->holds_seen = stream->holds_written;
    energy_stream_barrier();
}

/** The time of pair `sequence`, the next: the decoder numbers every pair it completes. One out of
 *  step starts the timeline again from it and marks a gap, rather than guess. A hold the pair comes
 *  after moves it, and every later pair, to when the restarted timer brought it. */
ENERGY_STREAM_INLINE uint64_t energy_stream_time(struct energy_stream* stream, uint32_t sequence) {
    if (sequence != stream->next_sequence) {
        stream->gap |= ENERGY_STREAM_FLAG_HELD;
    }
    while (stream->holds_read != stream->holds_seen) {
        energy_stream_hold_t const hold = stream->holds[stream->holds_read % ENERGY_STREAM_HOLDS];
        // The trigger due after the stop, found within half a period of the stamp's estimate.
        if ((int32_t)(stream->next_raw - hold.due + stream->period / 2U) < 0) {
            break;
        }
        stream->next_counts += hold.came - stream->next_raw;
        stream->next_raw = hold.came;
        stream->holds_read++;
        stream->gap |= ENERGY_STREAM_FLAG_HELD;
    }
    uint64_t const counts = stream->next_counts;
    stream->next_sequence = sequence + 1U;
    stream->next_raw += stream->period;
    stream->next_counts += stream->period;
    return counts;
}

/** A pair, as the owner integrates it: timed always, so a stream started mid-capture knows the
 *  time; appended while a stream runs. */
ENERGY_STREAM_INLINE void energy_stream_pair(struct energy_stream* stream, uint32_t sequence, uint16_t voltage,
                                             uint16_t current, bool low) {
    uint64_t const counts = energy_stream_time(stream, sequence);
    if (!stream->active) {
        return;
    }
    // A message never spans a pause or a hold, nor more than 10 ms of pairs.
    if ((stream->open_pairs != 0U) && ((stream->gap != 0U) || ((counts - stream->open_counts) >= stream->clock_hz / 100U))) {
        energy_stream_close(stream);
    }
    if (stream->open_pairs == 0U) {
        stream->open_dropped = energy_stream_free(stream) <= ENERGY_STREAM_CONTROL_SLOTS;
        stream->open_counts = counts;
        if (!stream->open_dropped) {
            uint8_t* out = energy_stream_slot(stream);
            *out++ = ENERGY_STREAM_PAIRS;
            *out++ = stream->gap;
            out = energy_stream_u32(out, sequence);
            (void)energy_stream_u64(out, energy_stream_ns(counts, stream->clock_hz));
            // A message that never leaves leaves its flags to the next one that does.
            stream->gap = 0U;
        }
    }
    if (!stream->open_dropped) {
        uint8_t* out = energy_stream_slot(stream) + ENERGY_STREAM_PAIRS_FIXED + stream->open_pairs * ENERGY_STREAM_PAIR_BYTES;
        out = energy_stream_u16(out, voltage);
        out = energy_stream_u16(out, current);
        *out = low ? 1U : 0U;
    }
    stream->open_pairs++;
    if (stream->open_pairs == ENERGY_STREAM_PAIRS_MAX) {
        energy_stream_close(stream);
    }
}

/** After each batch of pairs the owner integrates: a message whose next pair would take it past
 *  10 ms leaves now rather than at the next batch. */
ENERGY_STREAM_INLINE void energy_stream_batch_end(struct energy_stream* stream) {
    if (stream->active && (stream->open_pairs != 0U) &&
        ((stream->next_counts - stream->open_counts) >= stream->clock_hz / 100U)) {
        energy_stream_close(stream);
    }
}

#endif
