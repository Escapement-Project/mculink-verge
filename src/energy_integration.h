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
 * disabled, its INTA bit cleared, until the start enables it last. From then on only CPU 0's DMA
 * interrupt advances the state, and CPU 1 reads it under energy_accumulator_sequence.
 */
#ifndef ENERGY_INTEGRATION_H
#define ENERGY_INTEGRATION_H

#include <stdbool.h>
#include <stdint.h>

#include "fsl_device_registers.h"

#include "dap_shared.h"
#include "energy_monitor.h"
#include "energy_pairs.h"
#include "energy_samples.h"


/** Records a DMA error or an ADC FIFO overflow in dap_shared.energy_dma_error, which stays set
 *  until a new session starts; returns whether one is set. */
static inline bool energy_capture_failed(const struct energy_integration* energy) {
    if (((DMA0->COMMON[0].ERRINT & ENERGY_DMA_MASK) != 0U) ||
        (energy->running && ((ADC0->STAT & ADC_STAT_FOF0_MASK) != 0U))) {
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
    return (energy_total_t){dap_shared.energy_accumulator_pj, dap_shared.energy_accumulator_pairs,
                            dap_shared.energy_accumulator_fraction_q32};
}

static inline void energy_accumulator_publish(const energy_total_t* total) {
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
    energy_stream_batch_begin(&energy->stream);
    energy_accumulator_write_begin();
    energy_total_t total = energy_accumulator_values();
    uint32_t index = (energy->consumed_bytes / sizeof(uint32_t)) & (ring_words - 1U);
    uint32_t left = bytes / sizeof(uint32_t);
    // To the ring's end, then on from its start.
    while (left != 0U) {
        uint32_t const run = left < (ring_words - index) ? left : (ring_words - index);
        volatile uint32_t const* word = &energy->ring[index];
        volatile uint32_t const* const end = word + run;
        while (word != end) {
            energy_consume_result(energy, *word, &total);
            word++;
        }
        left -= run;
        index = 0U;
    }
    energy->consumed_bytes = written;
    energy_accumulator_publish(&total);
    energy_accumulator_write_end();
    energy_stream_batch_end(&energy->stream);
    return true;
}

/** CPU 0's DMA interrupt, for each completed segment before it counts it: integrates the segment,
 *  and asks for the low range back when every sample in it stayed on the high range under the
 *  setting's limit. A segment that completed no pair says nothing either way. */
static inline void energy_integrate(struct energy_integration* energy) {
    if (!energy->running) {
        return;
    }
    if (energy_capture_failed(energy)) {
        energy_stream_end(&energy->stream, ENERGY_STREAM_END_FAULT, energy->decoder.sequence);
        return;
    }
    uint32_t const completed = dap_shared.energy_segments - energy->segments_at_start + 1U;
    uint32_t const segment_bytes = (energy_ring_words(energy) / ENERGY_SEGMENTS) * sizeof(uint32_t);
    if (energy_consume_until(energy, completed * segment_bytes) && energy->on_high_range && !energy->rearm_blocked) {
        dap_shared.energy_range_retry_pending = 1U;
    }
    energy->on_high_range = false;
    energy->rearm_blocked = false;
}

#endif
