/** Verge Aux's energy stream (docs/host-interface.md, "The energy stream"): the messages, the
 *  timeline that gives each pair its time, the DIO edges, and the ring CPU 0 sends from. Free of
 *  hardware, so tools/energy_stream_test.c runs it on the host.
 *
 * The core that owns the capture state under the quiet handshake (src/energy_integration.h) writes
 * all of it, with four exceptions. CPU 1 records holds (`holds_written`) and a start's first
 * trigger (`timeline_pending`) while CPU 0 may own the rest. CPU 0's pin interrupt records edges
 * (`edge_entries_written`, src/dio_edges.c) whichever core owns it. CPU 1 records markers
 * (`marks_written`, 0x94) whichever core owns it. CPU 0's loop reads published messages and moves
 * `sent`.
 *
 * Times are counts of the clock the conversion timer runs from. The timer that stamps them is 32
 * bits and wraps in under a minute at that clock, so every comparison here is between nearby
 * values: the next pair's trigger against a hold or an edge that has just happened, or a resume
 * against the pair before its pause.
 */
#ifndef ENERGY_STREAM_H
#define ENERGY_STREAM_H

#include <assert.h>
#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "energy_samples.h"
#include "energy_identity.h"

#define ENERGY_STREAM_SLOTS 16U
#define ENERGY_STREAM_MESSAGE_MAX 512U
#define ENERGY_STREAM_PAIRS_MAX 99U
#define ENERGY_STREAM_PAIR_BYTES 5U
/** A pair's last byte: read on the low range, and read on the high range too near the amplifier's
 *  floor to trust (`under_range_code`). */
#define ENERGY_STREAM_PAIR_LOW 1U
#define ENERGY_STREAM_PAIR_UNDER_RANGE 2U
#define ENERGY_STREAM_PAIRS_FIXED 16U
/** Format 3's fields, then step 5's: weight, flags and the current before
 *  (docs/host-interface.md). */
#define ENERGY_STREAM_HEADER_BYTES 79U
#define ENERGY_STREAM_ID_HEADER_BYTES (ENERGY_STREAM_HEADER_BYTES + ENERGY_IDENTITY_BYTES)
/** A DIO stream's header, format 5: the type, the format, the clock, the pins, their levels and the
 *  edge resolution. */
#define ENERGY_STREAM_DIO_HEADER_BYTES 10U
#define ENERGY_STREAM_END_BYTES 6U
/** A DIO stream's end: its time follows, as no pair marks it. */
#define ENERGY_STREAM_DIO_END_BYTES 14U
#define ENERGY_STREAM_HOLDS 8U
/** Pairs messages leave this many slots for the header and end messages a stream needs. */
#define ENERGY_STREAM_CONTROL_SLOTS 2U
/** How long a start may take to publish its first trigger once its pairs can arrive. */
#define ENERGY_STREAM_PENDING_POLLS 100000U
#define ENERGY_STREAM_EDGES_MAX 99U
#define ENERGY_STREAM_EDGE_BYTES 5U
#define ENERGY_STREAM_EDGES_FIXED 12U
/** DIO lost: the type, a zero flags byte, the two times and the levels. */
#define ENERGY_STREAM_DIO_LOST_BYTES 19U
/** What CPU 0's pin interrupt can record before the owner sends it. */
#define ENERGY_STREAM_EDGE_ENTRIES 256U
/** The most counts one edges message spans, so that its offsets fit 32 bits of nanoseconds and
 *  converting them fits 64 bits. */
#define ENERGY_STREAM_EDGE_SPAN (UINT32_C(1) << 26U)
/** Markers waiting to be sent, and the most one markers message holds. */
#define ENERGY_STREAM_MARKS 16U
#define ENERGY_STREAM_MARKS_MAX 11U
#define ENERGY_STREAM_MARKS_FIXED 3U
/** A marker's time, number and label length, before its label. */
#define ENERGY_STREAM_MARK_BYTES 13U
#define ENERGY_STREAM_LABEL_MAX 32U
/** How long the polls at a stream's end wait for CPU 1 to finish a marker it began. */
#define ENERGY_STREAM_MARK_POLLS 100000U

static_assert((ENERGY_STREAM_SLOTS & (ENERGY_STREAM_SLOTS - 1U)) == 0U, "slots index by a mask");
static_assert((ENERGY_STREAM_EDGE_ENTRIES & (ENERGY_STREAM_EDGE_ENTRIES - 1U)) == 0U, "entries index by a mask");
static_assert(ENERGY_STREAM_PAIRS_FIXED + ENERGY_STREAM_PAIRS_MAX * ENERGY_STREAM_PAIR_BYTES < ENERGY_STREAM_MESSAGE_MAX,
              "a pairs message fits a packet with no zero-length packet after it");
static_assert(ENERGY_STREAM_EDGES_FIXED + ENERGY_STREAM_EDGES_MAX * ENERGY_STREAM_EDGE_BYTES < ENERGY_STREAM_MESSAGE_MAX,
              "an edges message fits a packet with no zero-length packet after it");
static_assert((ENERGY_STREAM_MARKS & (ENERGY_STREAM_MARKS - 1U)) == 0U, "marks index by a mask");
static_assert(ENERGY_STREAM_MARKS_FIXED +
                      ENERGY_STREAM_MARKS_MAX * (ENERGY_STREAM_MARK_BYTES + ENERGY_STREAM_LABEL_MAX) <
                  ENERGY_STREAM_MESSAGE_MAX,
              "a markers message fits a packet with no zero-length packet after it");

/** CPU 0 runs these from SRAM, in its DMA interrupt and its loop; a copy a compiler kept out of
 *  line would land in flash. */
#define ENERGY_STREAM_INLINE static inline __attribute__((always_inline))

enum : uint8_t {
    ENERGY_STREAM_PAIRS = 0xA0U,
    ENERGY_STREAM_HEADER = 0xA1U,
    ENERGY_STREAM_END = 0xA2U,
    ENERGY_STREAM_EDGES = 0xA3U,
    ENERGY_STREAM_MARKS_MESSAGE = 0xA4U,
    ENERGY_STREAM_DIO_LOST = 0xA5U,
};

/** What 0x94 answers, as energy_stream_mark finds it, and 1 for no capture, which its caller finds. */
enum : uint8_t {
    ENERGY_STREAM_MARK_SENT = 0U,
    ENERGY_STREAM_MARK_NO_CAPTURE = 1U,
    ENERGY_STREAM_MARK_NO_STREAM = 2U,
    ENERGY_STREAM_MARK_FULL = 3U,
};

/** A markers message's flag: markers were lost before its first one. */
#define ENERGY_STREAM_MARKS_LOST 1U

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

/** An entry's flags: the interrupt dropped edges before this run, from `lost_from`; CPU 0's loop
 *  asked for this run to record the levels; and, from bit 4, the pins whose detectors the loop
 *  cleared since the last run. */
#define ENERGY_STREAM_ENTRY_LOST 1U
#define ENERGY_STREAM_ENTRY_SYNC 2U
#define ENERGY_STREAM_ENTRY_CLEARED_SHIFT 4U

/** What one run of CPU 0's pin interrupt found: when it ran, which pins' detectors had latched a
 *  rise and a fall since the last run, and the levels it read after clearing them. */
typedef struct {
    uint32_t stamp;
    uint32_t lost_from;
    uint8_t rise;
    uint8_t fall;
    uint8_t levels;
    uint8_t flags;
} energy_stream_edge_entry_t;

/** A marker as CPU 1 records it: the stamping timer as 0x94 ran, what the host sent, and whether
 *  CPU 1 refused one for a full record since the last it recorded in this stream. */
typedef struct {
    uint32_t stamp;
    uint32_t number;
    uint8_t length;
    bool lost;
    uint8_t label[ENERGY_STREAM_LABEL_MAX];
} energy_stream_mark_t;

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
    /** This batch's pairs go untimed (energy_stream_batch_begin), counted from `untimed_from`. */
    bool untimed;
    uint32_t untimed_from;
    uint32_t period;
    uint32_t clock_hz;
    /** A DIO stream (0x90 mode 15): no capture behind it and no pairs, so `next_raw` and
     *  `next_counts` are an origin that energy_stream_dio_advance moves on. */
    bool pairless;
    /** Set by a start or a resume while it owns the state, cleared once its timer runs and `next_raw`
     *  holds the first trigger. No pair can complete before then. */
    volatile bool timeline_pending;
    volatile uint32_t holds_written;
    volatile uint32_t holds_read;
    /** `holds_written` as the batch of pairs being integrated began. CPU 1 records a hold before the
     *  timer runs again, so every hold a batch's pairs come after is recorded by then. */
    uint32_t holds_seen;
    energy_stream_hold_t holds[ENERGY_STREAM_HOLDS];
    /** Nanoseconds a count, Q32, rounded down. */
    uint64_t ns_q32;

    /** DIO edges. CPU 0's pin interrupt records entries whichever core owns the rest, and the
     *  owner sends them as edges messages. */
    volatile uint32_t edge_entries_written;
    volatile uint32_t edge_entries_read;
    energy_stream_edge_entry_t edge_entries[ENERGY_STREAM_EDGE_ENTRIES];
    /** The pins the stream reports, zero without a stream: CPU 0's loop turns their pin interrupts
     *  on and off to match. */
    volatile uint8_t edge_mask;
    /** Counted by each start of a stream, so CPU 0's loop asks the interrupt for a run that records
     *  the levels with every detector cleared since the header. */
    volatile uint32_t edge_begins;
    /** Entries are skipped until that run's, the first sync stamped after `edge_header_raw`. */
    bool edge_waiting;
    uint32_t edge_header_raw;
    /** The levels after the last edge the stream reported. */
    uint8_t edge_levels;
    /** Pins whose last edge reported came between a run's clear and its level read: its latch is
     *  in the next run, already accounted for. */
    uint8_t edge_credit_rise;
    uint8_t edge_credit_fall;
    /** CPU 0's pin interrupt and loop are losing edges, since the stamp `edge_dropped_from`; the next
     *  entry the interrupt records says so (src/dio_edges.c). A loss lasts a rest or an SPI
     *  transfer, well inside the 2^31 counts energy_stream_counts_at takes. */
    volatile bool edge_dropped;
    volatile uint32_t edge_dropped_from;
    /** The counts of the last entry the owner took, or of the header: a loss starts after it, so
     *  it never shares a time with an edge already sent. */
    uint64_t edge_known;
    /** A DIO lost message not yet sent: edges from `edge_lost_from` to `edge_lost_to`, in counts,
     *  may be missing, and from then on the levels are `edge_lost_levels`. A later loss joins it,
     *  and no edges or pairs message leaves before it. */
    bool edge_lost;
    uint8_t edge_lost_levels;
    uint64_t edge_lost_from;
    uint64_t edge_lost_to;

    /** Markers. CPU 1 records them as it runs 0x94, whichever core owns the rest, and the owner
     *  sends them as markers messages. CPU 1 records only while `marking`, from a stream's header to
     *  its end, and holds `mark_busy` from before it looks until it has recorded, so the end waits
     *  for a marker CPU 1 has already said is in the stream. */
    volatile bool marking;
    volatile bool mark_busy;
    volatile uint32_t marks_written;
    volatile uint32_t marks_read;
    /** Counted by each start of a stream, on whichever core starts it. */
    volatile uint32_t mark_streams;
    /** CPU 1's own: that it refused a marker for a full record, and in which stream, so only the
     *  next marker of that stream says so. */
    bool mark_dropped;
    uint32_t mark_dropped_stream;
    /** ENERGY_STREAM_MARKS_LOST for the next markers message. */
    uint8_t mark_flags;
    energy_stream_mark_t marks[ENERGY_STREAM_MARKS];

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
    /** Valid only for a non-dropped open pairs message. */
    uint8_t* open_payload;
};

/** The singleton capture's payload queue. Independent stream objects cannot share it concurrently. */
extern uint8_t energy_stream_slots[ENERGY_STREAM_SLOTS][ENERGY_STREAM_MESSAGE_MAX];

/** What a stream's header carries besides its tables (docs/host-interface.md, "Header: 0xa1"). */
typedef struct {
    /** The stamping clock, and the pair period in its counts: a host weights and spaces pairs by
     *  their ratio, which no whole rate in pairs a second gives exactly. */
    uint32_t clock_hz;
    uint32_t period;
    uint8_t setting; // 1 for 350 mA, 2 for 50 mA, 0 unknown
    uint16_t full_scale_mv;
    uint32_t first_sequence;
    bool previous_low;
    energy_calibration_t const* low;
    energy_calibration_t const* high;
    uint8_t dio_mask;
    uint8_t dio_levels;
    /** The stamping timer just after `dio_levels` was read. */
    uint32_t dio_stamp;
    uint16_t edge_resolution_ns;
    energy_capture_id_t const* capture_id;
    /** Step 5: the previous pair's share of an up-switch's stand-in in 1/65536, whether a down-switch
     *  keeps its own reading, and the current through the sense resistor of the pair before the
     *  first one sent, where there is one (src/energy_pairs.h). */
    uint32_t switch_weight_q16;
    bool rearm_between_pairs;
    bool previous_known;
    uint32_t previous_na;
    /** The capture holds the high range (`0x90` mode 14). */
    bool high_range_only;
} energy_stream_header_t;

ENERGY_STREAM_INLINE void energy_stream_barrier(void) {
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

ENERGY_STREAM_INLINE uint8_t* energy_stream_u16(uint8_t* out, uint16_t value) {
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8U);
    return out + 2;
}

/** Every caller writes a slot, in SRAM4 (tools/image.py checks where they are). Pairs are five
 *  bytes, so most words land unaligned, and the firmware is built with no unaligned access
 *  (CMakeLists.txt). SRAM4 is Normal memory, where one unaligned STR is legal; left to the
 *  compiler, every pair in CPU 0's DMA interrupt costs a memcpy call or four byte stores. Never
 *  pass a pointer into USB SRAM. */
ENERGY_STREAM_INLINE uint8_t* energy_stream_u32(uint8_t* out, uint32_t value) {
#if defined(__arm__)
    typedef struct {
        uint8_t bytes[4];
    } energy_stream_word_t;
    __asm__("str %1, %0" : "=m"(*(energy_stream_word_t*)out) : "r"(value));
#else
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    value = __builtin_bswap32(value);
#endif
    __builtin_memcpy(out, &value, sizeof value);
#endif
    return out + 4;
}

ENERGY_STREAM_INLINE uint8_t* energy_stream_u64(uint8_t* out, uint64_t value) {
    out = energy_stream_u32(out, (uint32_t)value);
    return energy_stream_u32(out, (uint32_t)(value >> 32U));
}

/** `n / d` and its remainder, for a quotient under 2^32. The C library's 64-bit division runs from
 *  flash, and this runs in CPU 0's DMA interrupt, from SRAM; constant shifts keep it out of the
 *  library too. */
ENERGY_STREAM_INLINE uint32_t energy_stream_divide(uint64_t n, uint32_t d, uint32_t* remainder) {
    uint64_t r = n >> 32U;
    uint32_t low = (uint32_t)n;
    uint32_t q = 0U;
    // Unrolled, clang's copy runs to hundreds of bytes in each place it lands in CPU 0's SRAM. It
    // runs once a message, so the loop costs nothing that matters.
#ifdef __clang__
#pragma clang loop unroll(disable)
#endif
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
    return energy_stream_slots[stream->written & (ENERGY_STREAM_SLOTS - 1U)];
}

/** Hand the slot at `written` to CPU 0's loop. */
ENERGY_STREAM_INLINE void energy_stream_publish(struct energy_stream* stream, uint16_t length) {
    stream->length[stream->written & (ENERGY_STREAM_SLOTS - 1U)] = length;
    energy_stream_barrier();
    stream->written++;
}

/** Counts since the capture started at stamping-timer value `raw`, which must be within 2^31
 *  counts of the timeline's next pair; 0 for a stamp before the capture started. */
ENERGY_STREAM_INLINE uint64_t energy_stream_counts_at(struct energy_stream const* stream, uint32_t raw) {
    int64_t const counts = (int64_t)stream->next_counts + (int32_t)(raw - stream->next_raw);
    return (counts < 0) ? 0U : (uint64_t)counts;
}

/** `counts`, under ENERGY_STREAM_EDGE_SPAN, in nanoseconds, rounded down. */
ENERGY_STREAM_INLINE uint32_t energy_stream_edge_offset(struct energy_stream const* stream, uint32_t counts) {
    uint32_t ns = (uint32_t)(((uint64_t)counts * stream->ns_q32) >> 32U);
    // ns_q32 is rounded down, so the product can come out one short.
    if ((uint64_t)(ns + 1U) * stream->clock_hz <= (uint64_t)counts * 1000000000U) {
        ns++;
    }
    return ns;
}

/** The resolution a header gives for edges stamped at `clock_hz`. */
ENERGY_STREAM_INLINE uint16_t energy_stream_edge_resolution(uint32_t clock_hz) {
    return (uint16_t)((1000000000U + clock_hz - 1U) / clock_hz);
}

/** Sends the pairs message being built, then the edges and markers recorded since the last. It is
 *  the only way to send markers: the pairs message is built in place, in the slot any other message
 *  would take. Out of line, as one copy in CPU 0's SRAM serves every caller. */
void energy_stream_close(struct energy_stream* stream);
/** Sends the DIO lost message waiting, if the ring has more than `keep` slots free. */
void energy_stream_dio_lost_send(struct energy_stream* stream, uint32_t keep);
/** energy_stream_ns once for the flushes and a DIO stream's end: clang unrolls its divisions to
 *  over a kilobyte, and CPU 0's SRAM has no room for a copy in each. */
uint64_t energy_stream_flush_ns(uint64_t counts, uint32_t clock_hz);

/** As a stream ends: CPU 1 records no more markers, and any it has begun is recorded. */
void energy_stream_unmark(struct energy_stream* stream);

/** CPU 1, running 0x94 with a capture running: records a marker for the stream, stamped by `timer`
 *  once it is known to be the stream's, between its header and its end. Says whether it did
 *  (ENERGY_STREAM_MARK_*). */
static inline uint8_t energy_stream_mark(struct energy_stream* stream, volatile uint32_t const* timer,
                                         uint32_t number, uint8_t const* label, uint8_t length) {
    stream->mark_busy = true;
    energy_stream_barrier();
    uint8_t result = ENERGY_STREAM_MARK_NO_STREAM;
    if (stream->marking) {
        uint32_t const stamp = *timer;
        energy_stream_barrier();
        uint32_t const streams = stream->mark_streams;
        uint32_t const at = stream->marks_written;
        if ((at - stream->marks_read) >= ENERGY_STREAM_MARKS) {
            stream->mark_dropped = true;
            stream->mark_dropped_stream = streams;
            result = ENERGY_STREAM_MARK_FULL;
        } else {
            energy_stream_mark_t* const mark = &stream->marks[at & (ENERGY_STREAM_MARKS - 1U)];
            mark->stamp = stamp;
            mark->number = number;
            mark->length = length;
            mark->lost = stream->mark_dropped && (stream->mark_dropped_stream == streams);
            stream->mark_dropped = false;
            for (uint32_t i = 0U; i < length; i++) {
                mark->label[i] = label[i];
            }
            energy_stream_barrier();
            stream->marks_written = at + 1U;
            result = ENERGY_STREAM_MARK_SENT;
        }
    }
    energy_stream_barrier();
    stream->mark_busy = false;
    return result;
}

/** Whether markers have waited 10 ms by the stamping `timer`, which it reads only when some wait:
 *  then a capture has clocked it. The owner sends them then, if no pairs message has taken them. */
ENERGY_STREAM_INLINE bool energy_stream_marks_due(struct energy_stream const* stream, volatile uint32_t const* timer) {
    uint32_t const read = stream->marks_read;
    if (!stream->active || (stream->marks_written == read)) {
        return false;
    }
    energy_stream_barrier();
    return (*timer - stream->marks[read & (ENERGY_STREAM_MARKS - 1U)].stamp) >= (stream->clock_hz / 100U);
}

/** Whether the ring has room for a stream's header. A stream that has none does not start. */
ENERGY_STREAM_INLINE bool energy_stream_room(struct energy_stream* stream) {
    stream->open_pairs = 0U;
    stream->open_dropped = false;
    stream->gap = 0U;
    if (energy_stream_free(stream) == 0U) {
        stream->active = false;
        stream->edge_mask = 0U;
        return false;
    }
    return true;
}

/** Publishes the header the caller wrote, `length` bytes, and starts the stream: the pins in
 *  `dio_mask` from their `dio_levels`, read just before the stamping timer read `dio_stamp`, and the
 *  markers from now. */
ENERGY_STREAM_INLINE void energy_stream_opened(struct energy_stream* stream, uint16_t length, uint8_t dio_mask,
                                               uint8_t dio_levels, uint32_t dio_stamp) {
    energy_stream_publish(stream, length);
    // Whatever the pin interrupt recorded before is an earlier stream's.
    stream->edge_entries_read = stream->edge_entries_written;
    stream->edge_waiting = true;
    stream->edge_header_raw = dio_stamp;
    stream->edge_levels = dio_levels & dio_mask;
    stream->edge_lost = false;
    stream->edge_mask = dio_mask;
    stream->marks_read = stream->marks_written;
    stream->mark_flags = 0U;
    stream->mark_streams++;
    stream->active = true;
    energy_stream_barrier();
    stream->edge_begins++;
    stream->marking = true;
}

/** The first message of a capture's stream. Starts the stream only if the ring had room for it. */
ENERGY_STREAM_INLINE bool energy_stream_begin(struct energy_stream* stream, energy_stream_header_t const* header) {
    if (!energy_stream_room(stream)) {
        return false;
    }
    stream->pairless = false;
    uint8_t* out = energy_stream_slot(stream);
    *out++ = ENERGY_STREAM_HEADER;
    *out++ = header->capture_id != NULL ? 4U : 3U;
    out = energy_stream_u32(out, header->clock_hz);
    out = energy_stream_u32(out, header->period);
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
    out = energy_stream_u16(out, header->edge_resolution_ns);
    if (header->capture_id != NULL) {
        for (uint32_t i = 0U; i < ENERGY_IDENTITY_BYTES; i++) {
            *out++ = header->capture_id->bytes[i];
        }
    }
    out = energy_stream_u16(out, (uint16_t)header->switch_weight_q16);
    *out++ = (uint8_t)((header->rearm_between_pairs ? 1U : 0U) | (header->previous_known ? 2U : 0U) |
                       (header->high_range_only ? 4U : 0U));
    out = energy_stream_u32(out, header->previous_known ? header->previous_na : 0U);
    energy_stream_opened(stream, header->capture_id != NULL ? ENERGY_STREAM_ID_HEADER_BYTES : ENERGY_STREAM_HEADER_BYTES,
                         header->dio_mask, header->dio_levels, header->dio_stamp);
    return true;
}

/** Times convert at `clock_hz`. */
ENERGY_STREAM_INLINE void energy_stream_clock(struct energy_stream* stream, uint32_t clock_hz) {
    stream->clock_hz = clock_hz;
    uint32_t left = 0U;
    uint32_t const whole = energy_stream_divide(1000000000U, clock_hz, &left);
    uint32_t unused = 0U;
    stream->ns_q32 = ((uint64_t)whole << 32U) | energy_stream_divide((uint64_t)left << 32U, clock_hz, &unused);
}

/** A DIO stream's first message, header format 5, on a board without energy capture
 *  (docs/host-interface.md, "DIO streams"): no capture, no pairs, and times from `dio_stamp`. Its
 *  owner keeps it from then on, and moves its origin on with energy_stream_dio_advance. Starts the
 *  stream only if the ring had room for it. */
ENERGY_STREAM_INLINE bool energy_stream_begin_dio(struct energy_stream* stream, uint32_t clock_hz, uint8_t dio_mask,
                                                  uint8_t dio_levels, uint32_t dio_stamp) {
    if (!energy_stream_room(stream)) {
        return false;
    }
    stream->pairless = true;
    stream->next_sequence = 0U;
    stream->next_raw = dio_stamp;
    stream->next_counts = 0U;
    stream->period = 0U;
    energy_stream_clock(stream, clock_hz);
    stream->holds_read = stream->holds_written;
    stream->holds_seen = stream->holds_read;
    stream->timeline_pending = false;
    uint8_t* out = energy_stream_slot(stream);
    *out++ = ENERGY_STREAM_HEADER;
    *out++ = 5U;
    out = energy_stream_u32(out, clock_hz);
    *out++ = dio_mask;
    *out++ = (uint8_t)(dio_levels & dio_mask);
    (void)energy_stream_u16(out, (dio_mask != 0U) ? energy_stream_edge_resolution(clock_hz) : 0U);
    energy_stream_opened(stream, ENERGY_STREAM_DIO_HEADER_BYTES, dio_mask, dio_levels, dio_stamp);
    return true;
}

/** A DIO stream's origin moves to the stamping timer's `now`. Nothing else moves it, and a stamp
 *  converts only within 2^31 counts of it, so its owner calls this more often than that: 14 s at
 *  150 MHz. `now` is never before the origin. */
ENERGY_STREAM_INLINE void energy_stream_dio_advance(struct energy_stream* stream, uint32_t now) {
    stream->next_counts += (uint32_t)(now - stream->next_raw);
    stream->next_raw = now;
}

/** The last message of a stream, after the pairs it was building. A DIO stream's says when it
 *  ended: its owner moves its origin to then first (energy_stream_dio_advance), and the end comes
 *  no earlier than an edge or loss the stream sent. The pin interrupt can record an edge stamped
 *  after that move, and the flush here sends it. */
ENERGY_STREAM_INLINE void energy_stream_end(struct energy_stream* stream, uint8_t reason, uint32_t last_sequence) {
    if (!stream->active) {
        return;
    }
    energy_stream_unmark(stream);
    energy_stream_close(stream);
    // Edges lost when the stream ends may be missing to its end: the time its next pair was due.
    if ((stream->edge_mask != 0U) && !stream->edge_waiting && stream->edge_dropped) {
        uint64_t from = energy_stream_counts_at(stream, stream->edge_dropped_from);
        from = (from <= stream->edge_known) ? stream->edge_known + 1U : from;
        if (!stream->edge_lost) {
            stream->edge_lost = true;
            stream->edge_lost_from = from;
        }
        stream->edge_lost_to = (stream->next_counts > from) ? stream->next_counts : from + 1U;
        stream->edge_lost_levels = stream->edge_levels;
    }
    uint64_t ended = stream->next_counts;
    if (stream->pairless) {
        // Until its sync run, `edge_known` is an earlier stream's.
        ended = (!stream->edge_waiting && (stream->edge_known > ended)) ? stream->edge_known : ended;
        ended = (stream->edge_lost && (stream->edge_lost_to > ended)) ? stream->edge_lost_to : ended;
    }
    // One slot stays for the end message.
    energy_stream_dio_lost_send(stream, 1U);
    stream->edge_lost = false;
    stream->active = false;
    stream->edge_mask = 0U;
    if (energy_stream_free(stream) == 0U) {
        return;
    }
    uint8_t* const out = energy_stream_slot(stream);
    out[0] = ENERGY_STREAM_END;
    out[1] = reason;
    uint8_t* const after = energy_stream_u32(out + 2, last_sequence);
    if (stream->pairless) {
        (void)energy_stream_u64(after, energy_stream_flush_ns(ended, stream->clock_hz));
        energy_stream_publish(stream, ENERGY_STREAM_DIO_END_BYTES);
        return;
    }
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
    energy_stream_clock(stream, clock_hz);
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

/** Before the owner integrates a batch of pairs: a start's timeline, and the holds recorded so far.
 *  `sequence` is the decoder's count of pairs so far. Without a stream or a hold to place, the batch
 *  goes untimed: energy_stream_batch_end moves the timeline on by all its pairs at once, which is
 *  where timing them one at a time leaves it. */
ENERGY_STREAM_INLINE void energy_stream_batch_begin(struct energy_stream* stream, uint32_t sequence) {
    for (uint32_t polls = 0U; stream->timeline_pending && (polls < ENERGY_STREAM_PENDING_POLLS); polls++) {
    }
    energy_stream_barrier();
    stream->holds_seen = stream->holds_written;
    energy_stream_barrier();
    stream->untimed = !stream->active && !stream->timeline_pending && (stream->holds_read == stream->holds_seen);
    stream->untimed_from = sequence;
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

/** A pair, as the owner integrates it: timed here in a timed batch, and at the end of an untimed
 *  one (energy_stream_batch_begin), so a stream started mid-capture knows the time; appended while
 *  a stream runs. */
ENERGY_STREAM_INLINE void energy_stream_pair(struct energy_stream* stream, uint32_t sequence, uint16_t voltage,
                                             uint16_t current, uint8_t flags) {
    if (stream->untimed) {
        return;
    }
    uint64_t const counts = energy_stream_time(stream, sequence);
    if (!stream->active) {
        return;
    }
    // A message never spans a pause or a hold, nor more than 10 ms of pairs. One the full ring
    // dropped gives way as soon as there is room, and a gap does not end it: its flags wait for the
    // next message that leaves. Closing it at every pair while a gap waited cost CPU 0 a close a pair.
    if (stream->open_pairs != 0U) {
        bool const ends = stream->open_dropped ? (energy_stream_free(stream) > ENERGY_STREAM_CONTROL_SLOTS)
                                               : (stream->gap != 0U);
        if (ends || ((counts - stream->open_counts) >= stream->clock_hz / 100U)) {
            energy_stream_close(stream);
        }
    }
    if (stream->open_pairs == 0U) {
        // A DIO lost message waiting goes before any later pairs. It takes a slot whenever a pairs
        // message could, so a pairs message never overtakes it.
        if (stream->edge_lost) {
            energy_stream_dio_lost_send(stream, ENERGY_STREAM_CONTROL_SLOTS);
        }
        stream->open_dropped = energy_stream_free(stream) <= ENERGY_STREAM_CONTROL_SLOTS;
        stream->open_counts = counts;
        if (!stream->open_dropped) {
            uint8_t* out = energy_stream_slot(stream);
            stream->open_payload = out + ENERGY_STREAM_PAIRS_FIXED;
            *out++ = ENERGY_STREAM_PAIRS;
            *out++ = stream->gap;
            out = energy_stream_u32(out, sequence);
            (void)energy_stream_u64(out, energy_stream_ns(counts, stream->clock_hz));
            // A message that never leaves leaves its flags to the next one that does.
            stream->gap = 0U;
        }
    }
    if (!stream->open_dropped) {
        uint8_t* out = stream->open_payload + stream->open_pairs * ENERGY_STREAM_PAIR_BYTES;
        out = energy_stream_u32(out, (uint32_t)voltage | ((uint32_t)current << 16U));
        *out = flags;
    }
    stream->open_pairs++;
    if (stream->open_pairs == ENERGY_STREAM_PAIRS_MAX) {
        energy_stream_close(stream);
    }
}

/** After each batch of pairs the owner integrates, `sequence` pairs counted: an untimed batch's
 *  pairs move the timeline on, and a message whose next pair would take it past 10 ms leaves now
 *  rather than at the next batch. */
ENERGY_STREAM_INLINE void energy_stream_batch_end(struct energy_stream* stream, uint32_t sequence) {
    if (stream->untimed) {
        uint32_t const pairs = sequence - stream->untimed_from;
        if (pairs != 0U) {
            stream->next_raw += pairs * stream->period;
            stream->next_counts += (uint64_t)pairs * stream->period;
            stream->next_sequence = sequence + 1U;
        }
        stream->untimed = false;
    }
    if (stream->active && (stream->open_pairs != 0U) &&
        ((stream->next_counts - stream->open_counts) >= stream->clock_hz / 100U)) {
        energy_stream_close(stream);
    }
}

#endif
