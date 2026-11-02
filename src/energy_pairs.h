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
 *  range's table. Only the range after is known, so a pair that switched up is clean if the
 *  comparator switched before its conversions and a blend if during them, and its reading cannot
 *  tell which. It takes a stand-in: the next pair's current, less the previous pair's share. That
 *  share is half the part of a period the conversions span, the expected part of them still on the
 *  low range for a switch at any moment, so the total is right on average over many switches.
 *  CPU 0 re-arms the low range between pairs where the rate leaves room (src/main.c), so a pair
 *  that switched down is clean and keeps its own reading; at other rates it takes the next pair's
 *  current. A pair whose next pair switched too keeps its own, as the next may be a blend. */
static inline uint32_t energy_stand_in_na(uint32_t next_na, uint32_t before_na, uint32_t weight_q16) {
    return (uint32_t)(((uint64_t)next_na * (65536U - weight_q16) + (uint64_t)before_na * weight_q16 + 32768U) >> 16U);
}

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
    bool const before_known = energy->previous_known;
    uint32_t const before_na = energy->previous_na;
    energy->previous_known = true;
    energy->previous_na = shunt_na;
    if (!switched && !energy->held) {
        energy_add_pair(total, mv, shunt_na);
        return;
    }
    energy->previous_low = low;
    if (energy->held) {
        energy_add_pair(total, energy->held_mv,
                        switched ? energy->held_na
                                 : energy_stand_in_na(shunt_na, energy->held_before_na, energy->held_weight_q16));
        energy->held = false;
    }
    if (switched && !(low && energy->rearm_between_pairs)) {
        energy->held = true;
        energy->held_mv = mv;
        energy->held_na = shunt_na;
        energy->held_weight_q16 = (!low && before_known) ? energy->switch_weight_q16 : 0U;
        energy->held_before_na = before_na;
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
