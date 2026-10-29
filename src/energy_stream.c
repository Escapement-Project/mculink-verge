#include "energy_stream.h"

/** The one copy of what every close calls: on CPU 0 it runs in the DMA interrupt and the loop, so
 *  from SRAM. */
#ifdef CPU_LPC55S69JBD64_cm33_core0
#define ENERGY_STREAM_RAMFUNC __attribute__((section(".ramfunc")))
#else
#define ENERGY_STREAM_RAMFUNC
#endif

/** An edges message as a flush builds it, in the slot at `written`. */
typedef struct {
    uint64_t first_counts;
    uint16_t edges;
    bool dropped;
} energy_stream_edges_t;

ENERGY_STREAM_INLINE void energy_stream_edges_send(struct energy_stream* stream, energy_stream_edges_t* message) {
    if (message->edges != 0U) {
        if (message->dropped) {
            stream->edge_flags |= ENERGY_STREAM_EDGES_LOST;
        } else {
            (void)energy_stream_u16(energy_stream_slot(stream) + 10, message->edges);
            energy_stream_publish(stream,
                                  (uint16_t)(ENERGY_STREAM_EDGES_FIXED + message->edges * ENERGY_STREAM_EDGE_BYTES));
        }
    }
    message->edges = 0U;
}

/** One edge at `counts`, leaving the levels `stream->edge_levels`. */
ENERGY_STREAM_INLINE void energy_stream_edge(struct energy_stream* stream, energy_stream_edges_t* message,
                                             uint64_t counts) {
    if ((message->edges != 0U) && ((counts - message->first_counts) >= ENERGY_STREAM_EDGE_SPAN)) {
        energy_stream_edges_send(stream, message);
    }
    if (message->edges == 0U) {
        message->first_counts = counts;
        message->dropped = energy_stream_free(stream) <= ENERGY_STREAM_CONTROL_SLOTS;
        if (!message->dropped) {
            uint8_t* out = energy_stream_slot(stream);
            *out++ = ENERGY_STREAM_EDGES;
            *out++ = stream->edge_flags;
            (void)energy_stream_u64(out, energy_stream_ns(counts, stream->clock_hz));
            stream->edge_flags = 0U;
        }
    }
    if (!message->dropped) {
        uint8_t* const out =
            energy_stream_slot(stream) + ENERGY_STREAM_EDGES_FIXED + message->edges * ENERGY_STREAM_EDGE_BYTES;
        (void)energy_stream_u32(out, energy_stream_edge_offset(stream, (uint32_t)(counts - message->first_counts)));
        out[4] = stream->edge_levels;
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

ENERGY_STREAM_RAMFUNC void energy_stream_edges_flush(struct energy_stream* stream) {
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
            }
            if ((entry.flags & ENERGY_STREAM_ENTRY_LOST) != 0U) {
                energy_stream_edges_send(stream, &message);
                stream->edge_flags |= ENERGY_STREAM_EDGES_LOST;
            }
            // A cleared detector will not show the latch a credit waits for.
            uint8_t const cleared = (uint8_t)(entry.flags >> ENERGY_STREAM_ENTRY_CLEARED_SHIFT);
            stream->edge_credit_rise &= (uint8_t)~cleared;
            stream->edge_credit_fall &= (uint8_t)~cleared;
            uint64_t const counts = energy_stream_counts_at(stream, entry.stamp);
            for (uint32_t pin = 0U; pin < 4U; pin++) {
                uint8_t const bit = (uint8_t)(1U << pin);
                if ((mask & bit) == 0U) {
                    continue;
                }
                uint32_t const edges = energy_stream_pin_edges(stream, &entry, bit);
                for (uint32_t i = 0U; i < edges; i++) {
                    stream->edge_levels ^= bit;
                    energy_stream_edge(stream, &message, counts);
                }
            }
        }
        energy_stream_edges_send(stream, &message);
    }
    energy_stream_barrier();
    stream->edge_entries_read = written;
}
