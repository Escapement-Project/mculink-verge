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

/** The voltage channel halves node T (R73 and R75) on its way to VREFP's full scale. */
static inline uint32_t energy_voltage_mv(uint16_t code, uint32_t full_scale_mv) {
    return ((uint32_t)code * 2U * full_scale_mv) >> 16U;
}

/** CTRL_HIGH_CURR, converted after each pair: U18's Q, high while the low range is in. */
static inline bool energy_low_range(uint16_t range_code) {
    return range_code >= 32768U;
}

static inline void energy_consume_result(struct energy_integration* energy, uint32_t result, energy_total_t* total) {
    if (!energy_sample_feed(&energy->decoder, result) || !energy->calibrated) {
        return;
    }
    bool const low = energy_low_range(energy->decoder.last_range);
    uint32_t const shunt_na = energy_shunt_na(energy->decoder.last_current, low ? &energy->low_range : &energy->high_range);
    energy->on_high_range = !low;
    if (!low && (shunt_na >= energy->rearm_below_na)) {
        energy->rearm_blocked = true;
    }
    uint32_t const mv = energy_voltage_mv(energy->decoder.last_voltage, energy->full_scale_mv);
    energy_total_add(total, (uint64_t)mv * energy_target_na(shunt_na, mv), energy->scale_q32);
}

/** Integrates the ring up to `written`, a byte position. Only the core that owns the capture may
 *  call it. */
static inline bool energy_consume_until(struct energy_integration* energy, uint32_t written) {
    uint32_t const bytes = written - energy->consumed_bytes;
    if ((bytes > ENERGY_RING_BYTES) || ((bytes % sizeof(uint32_t)) != 0U)) {
        dap_shared.energy_dma_error = 1U;
        return false;
    }
    energy_accumulator_write_begin();
    energy_total_t total = energy_accumulator_values();
    while (energy->consumed_bytes != written) {
        uint32_t const index = (energy->consumed_bytes / sizeof(uint32_t)) % ENERGY_RING_WORDS;
        energy_consume_result(energy, energy->ring[index], &total);
        energy->consumed_bytes += sizeof(uint32_t);
    }
    energy_accumulator_publish(&total);
    energy_accumulator_write_end();
    return true;
}

/** CPU 0's DMA interrupt, for each completed segment before it counts it: integrates the segment,
 *  and asks for the low range back when every sample in it stayed on the high range under the
 *  setting's limit. */
static inline void energy_integrate(struct energy_integration* energy) {
    if (!energy->running || energy_capture_failed(energy)) {
        return;
    }
    uint32_t const completed = dap_shared.energy_segments - energy->segments_at_start + 1U;
    if (energy_consume_until(energy, completed * ENERGY_SEGMENT_BYTES) && energy->on_high_range &&
        !energy->rearm_blocked) {
        dap_shared.energy_range_retry_pending = 1U;
    }
    energy->rearm_blocked = false;
}

#endif
