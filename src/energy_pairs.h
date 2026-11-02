/** Energy capture's per-pair conversion over struct energy_integration (src/dap_shared.h), free of
 *  hardware so tools/energy_samples_test.c can run it on the host. src/energy_integration.h says who
 *  may call it when. */
#ifndef ENERGY_PAIRS_H
#define ENERGY_PAIRS_H

#include <stdbool.h>
#include <stdint.h>

#include "dap_shared.h"
#include "energy_samples.h"
#include "energy_stream.h"

/** The voltage channel halves node T (R73 and R75) on its way to VREFP's full scale. */
static inline uint32_t energy_voltage_mv(uint16_t code, uint32_t full_scale_mv) {
    return ((uint32_t)code * 2U * full_scale_mv) >> 16U;
}

/** CTRL_HIGH_CURR, converted after each pair: U18's Q, high while the low range is in. */
static inline bool energy_low_range(uint16_t range_code) {
    return range_code >= 32768U;
}

static inline void energy_add_pair(energy_total_t* total, uint32_t mv, uint32_t shunt_na) {
    energy_total_sum(total, (uint64_t)mv * energy_target_na(shunt_na, mv));
}

/** A sample converts the voltage and the current together, averaged over up to 128 conversions, then
 *  the range. A switch while the current converts blends codes from both ranges, read with the range
 *  after: on the way up, anywhere from the true reading to the low range's code read with the high
 *  range's table. So a pair whose range differs from the previous pair's takes the next pair's
 *  current, unless that pair switched too and may be a blend itself; then it keeps its own. */
static inline void energy_consume_result(struct energy_integration* energy, uint32_t result, energy_total_t* total) {
    if (!energy_sample_feed(&energy->decoder, result)) {
        return;
    }
    energy_stream_pair(&energy->stream, energy->decoder.sequence, energy->decoder.last_voltage,
                       energy->decoder.last_current, energy_low_range(energy->decoder.last_range));
    if (!energy->calibrated) {
        return;
    }
    bool const low = energy_low_range(energy->decoder.last_range);
    uint32_t const shunt_na = energy_shunt_na(energy->decoder.last_current, low ? &energy->low_range : &energy->high_range);
    energy->on_high_range = !low;
    if (!low && (shunt_na >= energy->rearm_below_na)) {
        energy->rearm_blocked = true;
    }
    uint32_t const mv = energy_voltage_mv(energy->decoder.last_voltage, energy->full_scale_mv);
    bool const switched = low != energy->previous_low;
    if (!switched && !energy->held) {
        energy_add_pair(total, mv, shunt_na);
        return;
    }
    energy->previous_low = low;
    if (energy->held) {
        energy_add_pair(total, energy->held_mv, switched ? energy->held_na : shunt_na);
        energy->held = false;
    }
    if (switched) {
        energy->held = true;
        energy->held_mv = mv;
        energy->held_na = shunt_na;
        return;
    }
    energy_add_pair(total, mv, shunt_na);
}

/** A pair still waiting for the next one when capture pauses: its own reading is all there is. */
static inline void energy_flush_held(struct energy_integration* energy, energy_total_t* total) {
    if (energy->held) {
        energy_add_pair(total, energy->held_mv, energy->held_na);
        energy->held = false;
    }
}

#endif
