#ifndef ENERGY_SAMPLES_H
#define ENERGY_SAMPLES_H

#include <stdbool.h>
#include <stdint.h>

/** Current-channel codes against the current through the shunt, in increasing order. */
typedef struct {
    uint16_t code[4];
    uint32_t current_na[4];
    uint32_t slope_na_per_code_q16[3];
    uint8_t count;
} energy_calibration_t;

/** R73 and R75, the voltage channel's divider, sit on the target side of the shunts (PRO.md), so
 *  their current is in every current reading: 5 uA at 3.3 V. */
#define ENERGY_DIVIDER_OHMS 660000U

typedef enum : uint8_t {
    ENERGY_EXPECT_VOLTAGE,
    ENERGY_EXPECT_CURRENT,
    ENERGY_EXPECT_RANGE,
} energy_sample_phase_t;

typedef struct {
    energy_sample_phase_t phase;
    uint16_t voltage;
    uint16_t current;
    uint16_t last_voltage;
    uint16_t last_current;
    uint16_t last_range;
    uint32_t sequence;
    bool have_sample;
} energy_sample_decoder_t;

typedef struct {
    uint64_t pj;
    uint64_t pairs;
    uint32_t fraction_q32;
} energy_total_t;

static inline bool energy_sample_feed(energy_sample_decoder_t* decoder, uint32_t result) {
    if ((result & 0x80000000U) == 0U || ((result >> 16U) & 15U) != 7U) {
        return false;
    }
    uint32_t const command = (result >> 24U) & 15U;
    if (command == 1U) {
        decoder->voltage = (uint16_t)result;
        decoder->phase = ENERGY_EXPECT_CURRENT;
    } else if (command == 2U && decoder->phase == ENERGY_EXPECT_CURRENT) {
        decoder->current = (uint16_t)result;
        decoder->phase = ENERGY_EXPECT_RANGE;
    } else if (command == 3U && decoder->phase == ENERGY_EXPECT_RANGE) {
        decoder->last_voltage = decoder->voltage;
        decoder->last_current = decoder->current;
        decoder->last_range = (uint16_t)result;
        decoder->sequence++;
        decoder->have_sample = true;
        decoder->phase = ENERGY_EXPECT_VOLTAGE;
        return true;
    }
    return false;
}

static inline uint32_t energy_shunt_na(uint16_t code, const energy_calibration_t* calibration) {
    uint32_t segment = 0U;
    while ((segment + 2U) < calibration->count && code >= calibration->code[segment + 1U]) {
        segment++;
    }
    int64_t const code_delta = (int32_t)code - calibration->code[segment];
    int64_t const correction = code_delta * calibration->slope_na_per_code_q16[segment] / 65536;
    int64_t const current = calibration->current_na[segment] + correction;
    return current <= 0 ? 0U : current > UINT32_MAX ? UINT32_MAX : (uint32_t)current;
}

/** Nanoamps per millivolt through the divider, Q16. A multiply, because this runs per sample. */
#define ENERGY_DIVIDER_NA_PER_MV_Q16 ((uint32_t)((UINT64_C(65536000000) + ENERGY_DIVIDER_OHMS / 2U) / ENERGY_DIVIDER_OHMS))

/** The target's current: the shunt's, less the divider's. */
static inline uint32_t energy_target_na(uint32_t shunt_na, uint32_t voltage_mv) {
    uint32_t const divider_na = (uint32_t)(((uint64_t)voltage_mv * ENERGY_DIVIDER_NA_PER_MV_Q16) >> 16U);
    return shunt_na <= divider_na ? 0U : shunt_na - divider_na;
}

/** Adds one sample's power, in picowatts (millivolts times nanoamps). scale_q32 is
 *  floor(2^32 / sample_rate), with a rate of at least one sample/s. */
static inline void energy_total_add(energy_total_t* total, uint64_t power_pw, uint64_t scale_q32) {
    uint64_t const low = (uint64_t)(uint32_t)power_pw * scale_q32;
    uint64_t increment = (power_pw >> 32U) * scale_q32 + (low >> 32U);
    uint32_t const previous = total->fraction_q32;
    total->fraction_q32 += (uint32_t)low;
    increment += total->fraction_q32 < previous;
    total->pj = UINT64_MAX - total->pj < increment ? UINT64_MAX : total->pj + increment;
    if (total->pairs != UINT64_MAX) {
        total->pairs++;
    }
}

#endif
