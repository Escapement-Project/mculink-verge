#include <assert.h>
#include <stdint.h>

#include "energy_samples.h"

static uint32_t word(uint32_t command, uint16_t code) {
    return 0x80070000U | (command << 24U) | code;
}

int main(void) {
    energy_sample_decoder_t decoder = {0};
    for (uint32_t i = 0U; i < 4096U; i++) {
        assert(!energy_sample_feed(&decoder, 0U));
        assert(!energy_sample_feed(&decoder, 0x81000001U));
    }
    assert(!energy_sample_feed(&decoder, word(2U, 10U)));
    assert(!energy_sample_feed(&decoder, word(3U, 20U)));
    assert(!energy_sample_feed(&decoder, word(1U, 32768U)));
    assert(!energy_sample_feed(&decoder, 0U));
    assert(!energy_sample_feed(&decoder, word(2U, 123U)));
    assert(energy_sample_feed(&decoder, word(3U, 32768U)));
    assert(decoder.sequence == 1U && decoder.have_sample);
    assert(decoder.last_voltage == 32768U && decoder.last_current == 123U && decoder.last_range == 32768U);
    decoder.sequence = UINT32_MAX;
    assert(!energy_sample_feed(&decoder, word(1U, 1U)));
    decoder.phase = ENERGY_EXPECT_VOLTAGE;
    assert(!energy_sample_feed(&decoder, word(2U, 2U)));
    assert(!energy_sample_feed(&decoder, word(3U, 3U)));
    assert(!energy_sample_feed(&decoder, word(1U, 4U)));
    assert(!energy_sample_feed(&decoder, word(2U, 5U)));
    assert(energy_sample_feed(&decoder, word(3U, 6U)));
    assert(decoder.sequence == 0U && decoder.have_sample && decoder.last_voltage == 4U);

    energy_total_t total = {0};
    energy_total_add(&total, 3300U, 1000U, 4194304U);
    assert(total.nj == 3222U && total.fraction_q32 == 0xa8000000U && total.pairs == 1U);
    for (uint32_t i = 1U; i < 32U; i++) { energy_total_add(&total, 3300U, 1000U, 4194304U); }
    assert(total.nj == 103125U && total.fraction_q32 == 0U && total.pairs == 32U);
    total = (energy_total_t){0};
    energy_total_add(&total, 3300U, 1000U, 4294967U);
    assert(total.nj == 3299U && total.fraction_q32 == 0xfff11860U);
    uint64_t const prior = total.nj;
    energy_total_add(&total, 6600U, 0U, 4294967U);
    assert(total.nj == prior && total.pairs == 2U);
    total = (energy_total_t){0};
    energy_total_add(&total, UINT32_MAX, UINT32_MAX, UINT64_C(1) << 32U);
    assert(total.nj == (uint64_t)UINT32_MAX * UINT32_MAX && total.fraction_q32 == 0U);
    total = (energy_total_t){UINT64_MAX - 1U, UINT64_MAX, UINT32_MAX};
    energy_total_add(&total, 3300U, 1000U, 4194304U);
    assert(total.nj == UINT64_MAX && total.pairs == UINT64_MAX);

    energy_calibration_t const calibration = {
        .code = {10U, 20U, 30U}, .current_ua = {0U, 1000U, 3000U},
        .slope_ua_per_code_q16 = {100U * 65536U, 200U * 65536U}, .count = 3U,
    };
    assert(energy_current_ua(0U, &calibration) == 0);
    assert(energy_current_ua(15U, &calibration) == 500);
    assert(energy_current_ua(20U, &calibration) == 1000);
    assert(energy_current_ua(25U, &calibration) == 2000);
    assert(energy_current_ua(35U, &calibration) == 4000);
    energy_calibration_t const fractional = {
        .code = {10U, 20U}, .current_ua = {1000U, 1005U},
        .slope_ua_per_code_q16 = {32768U}, .count = 2U,
    };
    assert(energy_current_ua(9U, &fractional) == 1000);
    energy_calibration_t const large = {
        .code = {0U, 65535U}, .slope_ua_per_code_q16 = {UINT32_MAX}, .count = 2U,
    };
    assert(energy_current_ua(65535U, &large) == INT32_MAX);

    uint32_t const rates[] = {1U, 10U, 1000U, 1024U, 100000U};
    uint32_t seed = 19U;
    for (uint32_t rate = 0U; rate < sizeof(rates) / sizeof(rates[0]); rate++) {
        total = (energy_total_t){0};
        __uint128_t exact = 0U;
        uint64_t const scale = (UINT64_C(1) << 32U) / rates[rate];
        for (uint32_t i = 0U; i < 200U; i++) {
            seed = seed * 1664525U + 1013904223U;
            uint32_t const voltage = seed;
            seed = seed * 1664525U + 1013904223U;
            uint32_t const current = seed;
            exact += (__uint128_t)voltage * current * scale;
            energy_total_add(&total, voltage, current, scale);
            uint64_t const expected = exact >> 32U > UINT64_MAX ? UINT64_MAX : (uint64_t)(exact >> 32U);
            assert(total.nj == expected && total.fraction_q32 == (uint32_t)exact && total.pairs == i + 1U);
        }
    }
}
