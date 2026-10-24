#ifndef ENERGY_SAMPLES_H
#define ENERGY_SAMPLES_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint16_t code[4];
    uint32_t current_ua[4];
    uint32_t slope_ua_per_code_q16[3];
    uint8_t count;
} energy_calibration_t;

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
    uint64_t nj;
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

static inline int32_t energy_current_ua(uint16_t code, const energy_calibration_t* calibration) {
    uint32_t segment = 0U;
    while ((segment + 2U) < calibration->count && code >= calibration->code[segment + 1U]) {
        segment++;
    }
    int64_t const code_delta = (int32_t)code - calibration->code[segment];
    int64_t const correction = code_delta * calibration->slope_ua_per_code_q16[segment] / 65536;
    int64_t const current = calibration->current_ua[segment] + correction;
    return current <= 0 ? 0 : current > INT32_MAX ? INT32_MAX : (int32_t)current;
}

/** scale_q32 is floor(2^32 / sample_rate), with a rate of at least one sample/s. */
static inline void energy_total_add(energy_total_t* total, uint32_t voltage_mv, uint32_t current_ua,
                                    uint64_t scale_q32) {
    uint64_t const power_nw = (uint64_t)voltage_mv * current_ua;
    uint64_t const low = (uint64_t)(uint32_t)power_nw * scale_q32;
    uint64_t increment = (power_nw >> 32U) * scale_q32 + (low >> 32U);
    uint32_t const previous = total->fraction_q32;
    total->fraction_q32 += (uint32_t)low;
    increment += total->fraction_q32 < previous;
    total->nj = UINT64_MAX - total->nj < increment ? UINT64_MAX : total->nj + increment;
    if (total->pairs != UINT64_MAX) {
        total->pairs++;
    }
}

#endif
