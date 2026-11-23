#include "energy_stream.h"

#include <string.h>

/** The one copy of what every close calls: on CPU 0 it runs in the DMA interrupt and the loop, so
 *  from SRAM. */
#ifdef CPU_LPC55S69JBD64_cm33_core0
#define ENERGY_STREAM_RAMFUNC __attribute__((section(".ramfunc")))
#else
#define ENERGY_STREAM_RAMFUNC
#endif

#ifndef CPU_LPC55S69JBD64_cm33_core1
#ifdef CPU_LPC55S69JBD64_cm33_core0
__attribute__((section(".stream_slots")))
#endif
alignas(4) uint8_t energy_stream_slots[ENERGY_STREAM_SLOTS][ENERGY_STREAM_MESSAGE_MAX];
#endif

ENERGY_STREAM_RAMFUNC __attribute__((noinline)) uint64_t energy_stream_flush_ns(uint64_t counts, uint32_t clock_hz) {
    return energy_stream_ns(counts, clock_hz);
}

/** An edges message as a flush builds it, in the slot at `written`. */
typedef struct {
    uint64_t first_counts;
    uint16_t edges;
    bool dropped;
} energy_stream_edges_t;

ENERGY_STREAM_INLINE void energy_stream_edges_send(struct energy_stream* stream, energy_stream_edges_t* message) {
    // A dropped message's edges have joined the DIO lost message waiting.
    if ((message->edges != 0U) && !message->dropped) {
        (void)energy_stream_u16(energy_stream_slot(stream) + 10, message->edges);
        energy_stream_publish(stream, (uint16_t)(ENERGY_STREAM_EDGES_FIXED + message->edges * ENERGY_STREAM_EDGE_BYTES));
    }
    message->edges = 0U;
}

/** Edges from `from` to `to`, in counts, may be missing; the levels after are `edge_levels` as they
 *  are now. Joins the DIO lost message waiting, if there is one. */
ENERGY_STREAM_INLINE void energy_stream_dio_lost_join(struct energy_stream* stream, uint64_t from, uint64_t to) {
    if (!stream->edge_lost) {
        stream->edge_lost = true;
        stream->edge_lost_from = from;
    }
    stream->edge_lost_to = to;
    stream->edge_lost_levels = stream->edge_levels;
}

ENERGY_STREAM_RAMFUNC __attribute__((noinline)) void energy_stream_dio_lost_send(struct energy_stream* stream,
                                                                                 uint32_t keep) {
    if (!stream->edge_lost || (energy_stream_free(stream) <= keep)) {
        return;
    }
    uint8_t* out = energy_stream_slot(stream);
    *out++ = ENERGY_STREAM_DIO_LOST;
    *out++ = 0U;
    out = energy_stream_u64(out, energy_stream_flush_ns(stream->edge_lost_from, stream->clock_hz));
    out = energy_stream_u64(out, energy_stream_flush_ns(stream->edge_lost_to, stream->clock_hz));
    *out = stream->edge_lost_levels;
    energy_stream_publish(stream, ENERGY_STREAM_DIO_LOST_BYTES);
    stream->edge_lost = false;
}

/** One edge at `counts`, leaving the levels `stream->edge_levels`. */
ENERGY_STREAM_INLINE void energy_stream_edge(struct energy_stream* stream, energy_stream_edges_t* message,
                                             uint64_t counts) {
    if ((message->edges != 0U) && ((counts - message->first_counts) >= ENERGY_STREAM_EDGE_SPAN)) {
        energy_stream_edges_send(stream, message);
    }
    if (message->edges == 0U) {
        message->first_counts = counts;
        // A loss before goes first; while it cannot, these edges join it.
        energy_stream_dio_lost_send(stream, ENERGY_STREAM_CONTROL_SLOTS);
        message->dropped = stream->edge_lost || (energy_stream_free(stream) <= ENERGY_STREAM_CONTROL_SLOTS);
        if (!message->dropped) {
            uint8_t* out = energy_stream_slot(stream);
            *out++ = ENERGY_STREAM_EDGES;
            *out++ = 0U;
            (void)energy_stream_u64(out, energy_stream_flush_ns(counts, stream->clock_hz));
        }
    }
    if (!message->dropped) {
        uint8_t* const out =
            energy_stream_slot(stream) + ENERGY_STREAM_EDGES_FIXED + message->edges * ENERGY_STREAM_EDGE_BYTES;
        (void)energy_stream_u32(out, energy_stream_edge_offset(stream, (uint32_t)(counts - message->first_counts)));
        out[4] = stream->edge_levels;
    } else {
        // Its run's edges came after the last run the stream took, and the levels are known again
        // from this edge on.
        energy_stream_dio_lost_join(stream, stream->edge_known + 1U, counts);
    }
    message->edges++;
    if (message->edges == ENERGY_STREAM_EDGES_MAX) {
        energy_stream_edges_send(stream, message);
    }
}

/** One pin's edges from an entry: the latches the credit has not already reported, then one more if
 *  the level read after them differs, which leaves a credit for its latch. */
ENERGY_STREAM_INLINE uint32_t energy_stream_pin_edges(struct energy_stream* stream, energy_stream_edge_entry_t const* entry,
                                                      uint8_t bit) {
    bool const rise = ((entry->rise & bit) != 0U) && ((stream->edge_credit_rise & bit) == 0U);
    bool const fall = ((entry->fall & bit) != 0U) && ((stream->edge_credit_fall & bit) == 0U);
    stream->edge_credit_rise &= (uint8_t)~bit;
    stream->edge_credit_fall &= (uint8_t)~bit;
    bool const high = (stream->edge_levels & bit) != 0U;
    // A lone latch toward where the pin already is had a crossing the other way the detectors
    // merged: a pulse there and back.
    uint32_t edges = (rise && fall) ? 2U : (rise ? (high ? 2U : 1U) : (fall ? (high ? 1U : 2U) : 0U));
    bool const ends_high = high != ((edges & 1U) != 0U);
    if (ends_high != ((entry->levels & bit) != 0U)) {
        edges++;
        if (ends_high) {
            stream->edge_credit_fall |= bit;
        } else {
            stream->edge_credit_rise |= bit;
        }
    }
    return edges;
}

/** Sends what the pin interrupt has recorded, after the pairs message just closed, so every edge
 *  up to a pairs message's last pair leaves before the next pairs message. A stream's edges start
 *  at the run CPU 0's loop asks for once it begins, which records the levels with the detectors
 *  cleared after the header; a pin that moved before then shows the move at that run.
 *
 * A run reads each pin's rise and fall detectors, clears what it read, then reads the levels. An
 * edge between the clear and the level read is in the levels but latched for the next run, so it
 * is reported now and its latch, when it comes, is not counted again (`edge_credit_*`). Otherwise
 * each latch is an edge, both together a pulse there and back, and the levels decide the rest. A
 * detector holds one edge of each kind, so pulses closer together than the interrupt takes merge,
 * but the levels always end where the pins are. */
ENERGY_STREAM_RAMFUNC static void energy_stream_edges_flush(struct energy_stream* stream) {
    uint32_t const written = stream->edge_entries_written;
    energy_stream_barrier();
    uint8_t const mask = stream->edge_mask;
    if (stream->active && (mask != 0U)) {
        energy_stream_edges_t message = {0};
        for (uint32_t read = stream->edge_entries_read; read != written; read++) {
            energy_stream_edge_entry_t const entry = stream->edge_entries[read & (ENERGY_STREAM_EDGE_ENTRIES - 1U)];
            if (stream->edge_waiting) {
                if (((entry.flags & ENERGY_STREAM_ENTRY_SYNC) == 0U) ||
                    ((int32_t)(entry.stamp - stream->edge_header_raw) <= 0)) {
                    continue;
                }
                stream->edge_waiting = false;
                stream->edge_credit_rise = 0U;
                stream->edge_credit_fall = 0U;
                stream->edge_known = energy_stream_counts_at(stream, stream->edge_header_raw);
            }
            // After a loss the run's latches still move the levels and the credits, but the probe
            // cannot say when: the edges go out as one DIO lost message, to this run.
            bool const lost = (entry.flags & ENERGY_STREAM_ENTRY_LOST) != 0U;
            if (lost) {
                energy_stream_edges_send(stream, &message);
            }
            // A cleared detector will not show the latch a credit waits for.
            uint8_t const cleared = (uint8_t)(entry.flags >> ENERGY_STREAM_ENTRY_CLEARED_SHIFT);
            stream->edge_credit_rise &= (uint8_t)~cleared;
            stream->edge_credit_fall &= (uint8_t)~cleared;
            uint64_t const counts = energy_stream_counts_at(stream, entry.stamp);
            // A run's edges share its stamp, so they go out together or are lost together: a loss
            // never shares a time with an edge sent. Each pin's count reads only its own level.
            uint32_t edges[4] = {0U};
            uint32_t total = 0U;
            for (uint32_t pin = 0U; pin < 4U; pin++) {
                uint8_t const bit = (uint8_t)(1U << pin);
                if ((mask & bit) != 0U) {
                    edges[pin] = energy_stream_pin_edges(stream, &entry, bit);
                    total += edges[pin];
                }
            }
            if (!lost && ((message.edges + total) > ENERGY_STREAM_EDGES_MAX)) {
                energy_stream_edges_send(stream, &message);
            }
            for (uint32_t pin = 0U; pin < 4U; pin++) {
                for (uint32_t i = 0U; i < edges[pin]; i++) {
                    stream->edge_levels ^= (uint8_t)(1U << pin);
                    if (!lost) {
                        energy_stream_edge(stream, &message, counts);
                    }
                }
            }
            if (lost) {
                // Edges are lost after the last run the stream took: its edges were all sent.
                uint64_t from = energy_stream_counts_at(stream, entry.lost_from);
                from = (from <= stream->edge_known) ? stream->edge_known + 1U : from;
                from = (from > counts) ? counts : from;
                energy_stream_dio_lost_join(stream, from, counts);
                energy_stream_dio_lost_send(stream, ENERGY_STREAM_CONTROL_SLOTS);
            }
            stream->edge_known = counts;
        }
        energy_stream_edges_send(stream, &message);
    }
    energy_stream_barrier();
    stream->edge_entries_read = written;
}

/** A markers message as a flush builds it, in the slot at `written`. */
typedef struct {
    uint16_t length;
    uint8_t marks;
    bool dropped;
} energy_stream_marks_t;

ENERGY_STREAM_INLINE void energy_stream_marks_send(struct energy_stream* stream, energy_stream_marks_t* message) {
    if (message->marks != 0U) {
        if (message->dropped) {
            stream->mark_flags |= ENERGY_STREAM_MARKS_LOST;
        } else {
            energy_stream_slot(stream)[2] = message->marks;
            energy_stream_publish(stream, message->length);
        }
    }
    message->marks = 0U;
}

/** Sends the markers CPU 1 has recorded, in as few messages as they fit. A marker stamped before a
 *  start's timeline has no time to give and is dropped, as the stream ends without one. */
ENERGY_STREAM_RAMFUNC static void energy_stream_marks_flush(struct energy_stream* stream) {
    uint32_t const written = stream->marks_written;
    energy_stream_barrier();
    if (stream->active && !stream->timeline_pending) {
        energy_stream_marks_t message = {0};
        for (uint32_t read = stream->marks_read; read != written; read++) {
            energy_stream_mark_t const* const mark = &stream->marks[read & (ENERGY_STREAM_MARKS - 1U)];
            uint8_t const length = (mark->length < ENERGY_STREAM_LABEL_MAX) ? mark->length : ENERGY_STREAM_LABEL_MAX;
            // A marker CPU 1 refused came just before this one: a message of its own says so.
            if (mark->lost) {
                energy_stream_marks_send(stream, &message);
                stream->mark_flags |= ENERGY_STREAM_MARKS_LOST;
            }
            if (message.marks == 0U) {
                message.dropped = energy_stream_free(stream) <= ENERGY_STREAM_CONTROL_SLOTS;
                message.length = ENERGY_STREAM_MARKS_FIXED;
                if (!message.dropped) {
                    uint8_t* const out = energy_stream_slot(stream);
                    out[0] = ENERGY_STREAM_MARKS_MESSAGE;
                    out[1] = stream->mark_flags;
                    stream->mark_flags = 0U;
                }
            }
            if (!message.dropped) {
                uint8_t* out = energy_stream_slot(stream) + message.length;
                uint64_t const counts = energy_stream_counts_at(stream, mark->stamp);
                out = energy_stream_u64(out, energy_stream_flush_ns(counts, stream->clock_hz));
                out = energy_stream_u32(out, mark->number);
                *out++ = length;
                memcpy(out, mark->label, length);
            }
            message.length = (uint16_t)(message.length + ENERGY_STREAM_MARK_BYTES + length);
            message.marks++;
            if (message.marks == ENERGY_STREAM_MARKS_MAX) {
                energy_stream_marks_send(stream, &message);
            }
        }
        energy_stream_marks_send(stream, &message);
    }
    energy_stream_barrier();
    stream->marks_read = written;
}

ENERGY_STREAM_RAMFUNC __attribute__((noinline)) void energy_stream_close(struct energy_stream* stream) {
    if ((stream->open_pairs != 0U) && !stream->open_dropped) {
        uint8_t* const message = energy_stream_slot(stream);
        (void)energy_stream_u16(message + 14, stream->open_pairs);
        energy_stream_publish(stream,
                              (uint16_t)(ENERGY_STREAM_PAIRS_FIXED + stream->open_pairs * ENERGY_STREAM_PAIR_BYTES));
    }
    stream->open_pairs = 0U;
    stream->open_dropped = false;
    energy_stream_edges_flush(stream);
    energy_stream_marks_flush(stream);
}

ENERGY_STREAM_RAMFUNC __attribute__((noinline)) void energy_stream_unmark(struct energy_stream* stream) {
    // CPU 1 either sees `marking` clear, or has its marker recorded before the caller's flush.
    stream->marking = false;
    energy_stream_barrier();
    for (uint32_t polls = 0U; stream->mark_busy && (polls < ENERGY_STREAM_MARK_POLLS); polls++) {
    }
    energy_stream_barrier();
}

/* Once a stream: begin, end, pause, the timeline, holds and marks. Out of line, as one copy
 * serves every caller; on CPU 0 in SRAM with the rest. */

/** Times convert at `clock_hz`. */
ENERGY_STREAM_INLINE void energy_stream_clock(struct energy_stream* stream, uint32_t clock_hz) {
    stream->clock_hz = clock_hz;
    uint32_t left = 0U;
    uint32_t const whole = energy_stream_divide(1000000000U, clock_hz, &left);
    uint32_t unused = 0U;
    stream->ns_q32 = ((uint64_t)whole << 32U) | energy_stream_divide((uint64_t)left << 32U, clock_hz, &unused);
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

ENERGY_STREAM_RAMFUNC __attribute__((noinline)) uint8_t energy_stream_mark(struct energy_stream* stream,
                                                                           volatile uint32_t const* timer,
                                                                           uint32_t number, uint8_t const* label,
                                                                           uint8_t length) {
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

ENERGY_STREAM_RAMFUNC __attribute__((noinline)) bool energy_stream_begin(struct energy_stream* stream,
                                                                         energy_stream_header_t const* header) {
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
    energy_stream_opened(stream,
                         header->capture_id != NULL ? ENERGY_STREAM_ID_HEADER_BYTES : ENERGY_STREAM_HEADER_BYTES,
                         header->dio_mask, header->dio_levels, header->dio_stamp);
    return true;
}

ENERGY_STREAM_RAMFUNC __attribute__((noinline)) bool energy_stream_begin_dio(struct energy_stream* stream,
                                                                             uint32_t clock_hz, uint8_t dio_mask,
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

ENERGY_STREAM_RAMFUNC __attribute__((noinline)) void energy_stream_end(struct energy_stream* stream, uint8_t reason,
                                                                       uint32_t last_sequence) {
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

ENERGY_STREAM_RAMFUNC __attribute__((noinline)) void energy_stream_pause(struct energy_stream* stream) {
    energy_stream_close(stream);
    stream->gap |= ENERGY_STREAM_FLAG_PAUSED;
}

ENERGY_STREAM_RAMFUNC __attribute__((noinline)) void energy_stream_timeline_prepare(struct energy_stream* stream,
                                                                                    uint32_t first_sequence) {
    stream->timeline_pending = true;
    stream->next_sequence = first_sequence;
    stream->holds_read = stream->holds_written;
    stream->holds_seen = stream->holds_read;
}

ENERGY_STREAM_RAMFUNC __attribute__((noinline)) void energy_stream_timeline_run(struct energy_stream* stream,
                                                                                bool new_session, uint32_t clock_hz,
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

ENERGY_STREAM_RAMFUNC __attribute__((noinline)) energy_stream_hold_t* energy_stream_hold(struct energy_stream* stream,
                                                                                         uint32_t due) {
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
