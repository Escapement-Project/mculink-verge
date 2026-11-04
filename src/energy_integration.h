/** Energy capture's integration, over struct energy_integration (src/dap_shared.h): the ring DMA0
 *  fills, the decoder and the position in it, and what CPU 1 publishes for each capture. The state
 *  lives in CPU 0's SRAM. CPU 0 integrates each completed segment from its DMA interrupt, running
 *  from SRAM; CPU 1 reaches the state through dap_shared.energy_integration, which CPU 0 sets
 *  before CPU 1 starts.
 *
 * The quiet handshake (dap_shared.energy_control_request) decides which core may change it. Once
 * CPU 0 has acknowledged quiet, CPU 1 owns all of it: it drains the tail, and a start writes the
 * tables, constants, descriptors and the position for the next capture. The start then releases
 * quiet, and only after that does a new session reset the accumulator, the decoder and the
 * position, and does the start set `running`. No completion can come meanwhile: the channel stays
 * disabled, its flags cleared, until the start enables it last. From then on only CPU 0's DMA
 * interrupt advances the state, and CPU 1 reads it under energy_accumulator_sequence.
 */
#ifndef ENERGY_INTEGRATION_H
#define ENERGY_INTEGRATION_H

#include <stdbool.h>
#include <stdint.h>

#include "fsl_device_registers.h"

#include "dap_shared.h"
#include "dma0.h"
#include "energy_monitor.h"
#include "energy_pairs.h"
#include "energy_samples.h"


/** The flag DMA0 raises as it completes segment `counted` of a capture: INTA for the ring's first
 *  half, INTB for the second (src/energy_monitor.c). */
static inline volatile uint32_t* energy_segment_flag(uint32_t counted) {
    return dma0_ring_flag(counted);
}

/** Records a DMA error, an ADC FIFO overflow or a segment CPU 0 has not taken by the time the next
 *  completes (both halves' flags up) in dap_shared.energy_dma_error, which stays set until a new
 *  session starts; returns whether one is set. */
static inline bool energy_capture_failed(const struct energy_integration* energy) {
    if (((DMA0->COMMON[0].ERRINT & ENERGY_DMA_MASK) != 0U) ||
        (energy->running && ((ADC0->STAT & ADC_STAT_FOF0_MASK) != 0U)) ||
        ((DMA0->COMMON[0].INTA & DMA0->COMMON[0].INTB & ENERGY_DMA_MASK) != 0U)) {
        dap_shared.energy_dma_error = 1U;
    }
    return dap_shared.energy_dma_error != 0U;
}

static inline void energy_accumulator_write_begin(void) {
    dap_shared.energy_accumulator_sequence++;
    __DMB();
}

static inline void energy_accumulator_write_end(void) {
    __DMB();
    dap_shared.energy_accumulator_sequence++;
}

static inline energy_total_t energy_accumulator_values(void) {
    return (energy_total_t){.pj = dap_shared.energy_accumulator_pj,
                            .pairs = dap_shared.energy_accumulator_pairs,
                            .fraction_q32 = dap_shared.energy_accumulator_fraction_q32};
}

/** Folds the batch's summed power in at `scale_q32`, then publishes. */
static inline void energy_accumulator_publish(energy_total_t* total, uint64_t scale_q32) {
    energy_total_fold(total, scale_q32);
    dap_shared.energy_accumulator_pj = total->pj;
    dap_shared.energy_accumulator_pairs = total->pairs;
    dap_shared.energy_accumulator_fraction_q32 = total->fraction_q32;
}

/** The words this capture's two segments use. Byte positions wrap at 2^32, so the ring has to be a
 *  power of two; the mask also keeps CPU 0 inside the array whatever CPU 1 left in segment_words. */
static inline uint32_t energy_ring_words(const struct energy_integration* energy) {
    return (((ENERGY_SEGMENTS * energy->segment_words) - 1U) & (ENERGY_RING_WORDS - 1U)) + 1U;
}

/** Integrates the ring up to `written`, a byte position. Only the core that owns the capture may
 *  call it. */
static inline bool energy_consume_until(struct energy_integration* energy, uint32_t written) {
    uint32_t const ring_words = energy_ring_words(energy);
    uint32_t const bytes = written - energy->consumed_bytes;
    if ((bytes > (ring_words * sizeof(uint32_t))) || ((bytes % sizeof(uint32_t)) != 0U)) {
        dap_shared.energy_dma_error = 1U;
        return false;
    }
    energy_stream_batch_begin(&energy->stream, energy->decoder.sequence);
    energy_accumulator_write_begin();
    energy_total_t total = energy_accumulator_values();
    uint32_t index = (energy->consumed_bytes / sizeof(uint32_t)) & (ring_words - 1U);
    uint32_t left = bytes / sizeof(uint32_t);
    // A copy the compiler can keep in registers. CPU 1 reads the decoder only under the sequence
    // count, which stays odd until it is written back below.
    energy_sample_decoder_t decoder = energy->decoder;
    // To the ring's end, then on from its start.
    while (left != 0U) {
        uint32_t const run = left < (ring_words - index) ? left : (ring_words - index);
        volatile uint32_t const* word = &energy->ring[index];
        volatile uint32_t const* const end = word + run;
        while (word != end) {
            if (energy_sample_feed(&decoder, *word)) {
                energy_consume_pair(energy, &decoder, &total);
            }
            word++;
        }
        left -= run;
        index = 0U;
    }
    energy->decoder = decoder;
    energy->consumed_bytes = written;
    energy_accumulator_publish(&total, energy->scale_q32);
    energy_accumulator_write_end();
    energy_stream_batch_end(&energy->stream, energy->decoder.sequence);
    return true;
}

/** CPU 0's DMA interrupt, for each completed segment before it counts it, with the channel's INTA
 *  and INTB as it took and cleared them: integrates the segment, and asks for the low range back
 *  when every sample in it stayed on the high range under the setting's limit. A segment that
 *  completed no pair says nothing either way.
 *
 *  The flag taken has to be this segment's alone, and the next segment's still down once this one
 *  is integrated: that completion is DMA0 starting on this half again. Either fails the capture. */
static inline void energy_integrate(struct energy_integration* energy, bool took_a, bool took_b) {
    if (!energy->running) {
        return;
    }
    uint32_t const counted = dap_shared.energy_segments - energy->segments_at_start;
    bool const first_half = (counted & 1U) == 0U;
    if ((took_a != first_half) || (took_b == first_half)) {
        dap_shared.energy_dma_error = 1U;
    }
    if (!energy_capture_failed(energy)) {
        uint32_t const segment_bytes = (energy_ring_words(energy) / ENERGY_SEGMENTS) * sizeof(uint32_t);
        if (energy_consume_until(energy, (counted + 1U) * segment_bytes) && energy->on_high_range &&
            !energy->rearm_blocked) {
            dap_shared.energy_range_retry_pending = 1U;
        }
        energy->on_high_range = false;
        energy->rearm_blocked = false;
        if ((*energy_segment_flag(counted + 1U) & ENERGY_DMA_MASK) != 0U) {
            dap_shared.energy_dma_error = 1U;
        }
    }
    if (dap_shared.energy_dma_error != 0U) {
        energy_stream_end(&energy->stream, ENERGY_STREAM_END_FAULT, energy->decoder.sequence);
    }
}

#endif
