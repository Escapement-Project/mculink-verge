#include <assert.h>
#include <stdint.h>

#include "energy_pairs.h"
#include "energy_samples.h"

static uint32_t word(uint32_t command, uint16_t code) {
    return 0x80070000U | (command << 24U) | code;
}

/** 30 nA a code on the low range and 6000 on the high, so a blended code read high shows. */
static uint32_t nanoamps(uint16_t code, bool low) {
    return (uint32_t)code * (low ? 30U : 6000U);
}

static void feed_pair(struct energy_integration* energy, energy_total_t* total, uint16_t voltage, uint16_t current,
                      bool low) {
    energy_consume_result(energy, word(1U, voltage), total);
    energy_consume_result(energy, word(1U, current), total);
    energy_consume_result(energy, word(2U, low ? 65535U : 0U), total);
}

static void expect_pair(energy_total_t* total, uint16_t voltage, uint32_t shunt_na) {
    uint32_t const mv = energy_voltage_mv(voltage, 3300U);
    energy_total_add(total, (uint64_t)mv * energy_target_na(shunt_na, mv), 4194304U);
}

/** src/energy_pairs.h: a pair whose range differs from the one before takes the next pair's
 *  current, unless that pair switched too; a pair still waiting at a pause keeps its own. */
static void test_range_switches(void) {
    static struct energy_integration energy;
    energy.low_range = (energy_calibration_t){
        .code = {0U, 65535U}, .current_na = {0U, 30U * 65535U}, .slope_na_per_code_q16 = {30U << 16U}, .count = 2U};
    energy.high_range = (energy_calibration_t){
        .code = {0U, 65535U}, .current_na = {0U, 6000U * 65535U}, .slope_na_per_code_q16 = {6000U << 16U}, .count = 2U};
    energy.scale_q32 = 4194304U;
    energy.full_scale_mv = 3300U;
    energy.rearm_below_na = UINT32_MAX;
    energy.calibrated = true;
    energy.previous_low = true; // as a start leaves it
    energy_total_t total = {0};
    energy_total_t expected = {0};

    feed_pair(&energy, &total, 30000U, 50000U, true);
    expect_pair(&expected, 30000U, nanoamps(50000U, true));
    // Up, with a blended code: it takes the next pair's current. A fold here, with the pair held
    // across it, changes nothing: folds come at every batch's end.
    feed_pair(&energy, &total, 31000U, 59000U, false);
    assert(total.pairs == 1U);
    energy_total_fold(&total, energy.scale_q32);
    feed_pair(&energy, &total, 32000U, 300U, false);
    expect_pair(&expected, 31000U, nanoamps(300U, false));
    expect_pair(&expected, 32000U, nanoamps(300U, false));
    // Down, then straight back up: the first keeps its own, the second takes the one after.
    feed_pair(&energy, &total, 33000U, 40000U, true);
    energy_total_fold(&total, energy.scale_q32);
    feed_pair(&energy, &total, 34000U, 59000U, false);
    feed_pair(&energy, &total, 35000U, 300U, false);
    expect_pair(&expected, 33000U, nanoamps(40000U, true));
    expect_pair(&expected, 34000U, nanoamps(300U, false));
    expect_pair(&expected, 35000U, nanoamps(300U, false));
    // Down on the last pair before a pause: its own reading.
    feed_pair(&energy, &total, 36000U, 20000U, true);
    assert(total.pairs == 6U);
    energy_flush_held(&energy, &total);
    expect_pair(&expected, 36000U, nanoamps(20000U, true));
    energy_total_fold(&total, energy.scale_q32);
    assert(total.pairs == 7U && total.pj == expected.pj && total.fraction_q32 == expected.fraction_q32);
    energy_flush_held(&energy, &total);
    assert(total.pairs == 7U);
}

/** The weighted stand-in at an up-switch: the next pair's current less the previous pair's share;
 *  with no pair before it, the next pair's. A down-switch between pairs keeps its own. */
static void test_weighted_switches(void) {
    static struct energy_integration energy;
    energy.low_range = (energy_calibration_t){
        .code = {0U, 65535U}, .current_na = {0U, 30U * 65535U}, .slope_na_per_code_q16 = {30U << 16U}, .count = 2U};
    energy.high_range = (energy_calibration_t){
        .code = {0U, 65535U}, .current_na = {0U, 6000U * 65535U}, .slope_na_per_code_q16 = {6000U << 16U}, .count = 2U};
    energy.scale_q32 = 4194304U;
    energy.full_scale_mv = 3300U;
    energy.rearm_below_na = UINT32_MAX;
    energy.calibrated = true;
    energy.previous_low = true;
    energy.switch_weight_q16 = 16384U; // a quarter
    energy.rearm_between_pairs = true;
    energy_total_t total = {0};
    energy_total_t expected = {0};

    // The first pair is high: nothing before it, so the next pair's current.
    feed_pair(&energy, &total, 30000U, 59000U, false);
    feed_pair(&energy, &total, 30100U, 400U, false);
    expect_pair(&expected, 30000U, nanoamps(400U, false));
    expect_pair(&expected, 30100U, nanoamps(400U, false));
    // Down between pairs: its own reading, at once.
    feed_pair(&energy, &total, 31000U, 7000U, true);
    assert(total.pairs == 3U && !energy.held);
    expect_pair(&expected, 31000U, nanoamps(7000U, true));
    feed_pair(&energy, &total, 31500U, 40000U, true);
    expect_pair(&expected, 31500U, nanoamps(40000U, true));
    // Up: three quarters of the next pair's current and a quarter of the one before, rounded.
    feed_pair(&energy, &total, 32000U, 59000U, false);
    feed_pair(&energy, &total, 33000U, 300U, false);
    uint64_t const mixed = ((uint64_t)nanoamps(300U, false) * 49152U + (uint64_t)nanoamps(40000U, true) * 16384U + 32768U) >> 16U;
    expect_pair(&expected, 32000U, (uint32_t)mixed);
    expect_pair(&expected, 33000U, nanoamps(300U, false));
    // A pause forgets the pair before: the next up-switch takes the next pair's current.
    energy_flush_held(&energy, &total);
    energy.previous_low = true;
    energy.previous_known = false;
    feed_pair(&energy, &total, 34000U, 59000U, false);
    feed_pair(&energy, &total, 35000U, 500U, false);
    expect_pair(&expected, 34000U, nanoamps(500U, false));
    expect_pair(&expected, 35000U, nanoamps(500U, false));
    // Without room between pairs, a down-switch takes the next pair's current again.
    energy.rearm_between_pairs = false;
    feed_pair(&energy, &total, 36000U, 3000U, true);
    feed_pair(&energy, &total, 37000U, 20000U, true);
    expect_pair(&expected, 36000U, nanoamps(20000U, true));
    expect_pair(&expected, 37000U, nanoamps(20000U, true));
    energy_total_fold(&total, energy.scale_q32);
    assert(total.pairs == 10U && total.pj == expected.pj && total.fraction_q32 == expected.fraction_q32);
    assert(energy_stand_in_na(UINT32_MAX, UINT32_MAX, 32768U) == UINT32_MAX);
    assert(energy_stand_in_na(1000U, 0U, 0U) == 1000U);
}

int main(void) {
    test_range_switches();
    test_weighted_switches();
    energy_sample_decoder_t decoder = {0};
    for (uint32_t i = 0U; i < 4096U; i++) {
        assert(!energy_sample_feed(&decoder, 0U));
        assert(!energy_sample_feed(&decoder, 0x81000001U));
    }
    // A range with no pair before it, and a command the capture does not use, are not a sample.
    assert(!energy_sample_feed(&decoder, word(2U, 10U)));
    assert(!energy_sample_feed(&decoder, word(3U, 20U)));
    // Command 1's two results: the voltage, then the current.
    assert(!energy_sample_feed(&decoder, word(1U, 32768U)));
    assert(!energy_sample_feed(&decoder, 0U));
    assert(!energy_sample_feed(&decoder, word(1U, 123U)));
    assert(!energy_sample_feed(&decoder, word(3U, 7U)));
    assert(energy_sample_feed(&decoder, word(2U, 32768U)));
    assert(decoder.sequence == 1U && decoder.have_sample);
    assert(decoder.last_voltage == 32768U && decoder.last_current == 123U && decoder.last_range == 32768U);
    decoder.sequence = UINT32_MAX;
    assert(!energy_sample_feed(&decoder, word(1U, 1U)));
    decoder.phase = ENERGY_EXPECT_VOLTAGE;
    assert(!energy_sample_feed(&decoder, word(2U, 2U)));
    assert(!energy_sample_feed(&decoder, word(1U, 4U)));
    assert(!energy_sample_feed(&decoder, word(1U, 5U)));
    assert(energy_sample_feed(&decoder, word(2U, 6U)));
    assert(decoder.sequence == 0U && decoder.have_sample && decoder.last_voltage == 4U && decoder.last_current == 5U);
    // The end of a pair this decoder joined late: its current and range, then a whole pair.
    assert(!energy_sample_feed(&decoder, word(1U, 100U)));
    assert(!energy_sample_feed(&decoder, word(2U, 101U)));
    assert(!energy_sample_feed(&decoder, word(1U, 102U)));
    assert(!energy_sample_feed(&decoder, word(1U, 103U)));
    assert(energy_sample_feed(&decoder, word(2U, 104U)));
    assert(decoder.last_voltage == 102U && decoder.last_current == 103U && decoder.last_range == 104U);
    // A pair whose range was not seen: its successor's voltage starts afresh.
    assert(!energy_sample_feed(&decoder, word(1U, 7U)));
    assert(!energy_sample_feed(&decoder, word(1U, 8U)));
    assert(!energy_sample_feed(&decoder, word(1U, 9U)));
    assert(!energy_sample_feed(&decoder, word(1U, 10U)));
    assert(energy_sample_feed(&decoder, word(2U, 11U)));
    assert(decoder.last_voltage == 9U && decoder.last_current == 10U);

    energy_total_t total = {0};
    energy_total_add(&total, 3300U * 1000U, 4194304U);
    assert(total.pj == 3222U && total.fraction_q32 == 0xa8000000U && total.pairs == 1U);
    for (uint32_t i = 1U; i < 32U; i++) { energy_total_add(&total, 3300U * 1000U, 4194304U); }
    assert(total.pj == 103125U && total.fraction_q32 == 0U && total.pairs == 32U);
    total = (energy_total_t){0};
    energy_total_add(&total, 3300U * 1000U, 4294967U);
    assert(total.pj == 3299U && total.fraction_q32 == 0xfff11860U);
    uint64_t const prior = total.pj;
    energy_total_add(&total, 0U, 4294967U);
    assert(total.pj == prior && total.pairs == 2U);
    total = (energy_total_t){0};
    energy_total_add(&total, (uint64_t)UINT32_MAX * UINT32_MAX, UINT64_C(1) << 32U);
    assert(total.pj == (uint64_t)UINT32_MAX * UINT32_MAX && total.fraction_q32 == 0U);
    total = (energy_total_t){.pj = UINT64_MAX - 1U, .pairs = UINT64_MAX, .fraction_q32 = UINT32_MAX};
    energy_total_add(&total, 3300U * 1000U, 4194304U);
    assert(total.pj == UINT64_MAX && total.pairs == UINT64_MAX);

    energy_calibration_t const calibration = {
        .code = {10U, 20U, 30U}, .current_na = {0U, 1000U, 3000U},
        .slope_na_per_code_q16 = {100U * 65536U, 200U * 65536U}, .count = 3U,
    };
    assert(energy_shunt_na(0U, &calibration) == 0U);
    assert(energy_shunt_na(15U, &calibration) == 500U);
    assert(energy_shunt_na(20U, &calibration) == 1000U);
    assert(energy_shunt_na(25U, &calibration) == 2000U);
    assert(energy_shunt_na(35U, &calibration) == 4000U);
    energy_calibration_t const fractional = {
        .code = {10U, 20U}, .current_na = {1000U, 1005U},
        .slope_na_per_code_q16 = {32768U}, .count = 2U,
    };
    assert(energy_shunt_na(9U, &fractional) == 1000U);
    energy_calibration_t const large = {
        .code = {0U, 65535U}, .current_na = {UINT32_MAX - 1000U}, .slope_na_per_code_q16 = {UINT32_MAX},
        .count = 2U,
    };
    assert(energy_shunt_na(65535U, &large) == UINT32_MAX);
    // A count past the four points a table holds, as a faulting CPU 1 could leave in CPU 0's copy,
    // reads as four: the sanitizers fail an index past the arrays.
    energy_calibration_t full = {
        .code = {10U, 20U, 30U, 40U}, .current_na = {0U, 1000U, 3000U, 6000U},
        .slope_na_per_code_q16 = {100U * 65536U, 200U * 65536U, 300U * 65536U}, .count = 4U,
    };
    uint32_t const four = energy_shunt_na(65535U, &full);
    full.count = 255U;
    assert(energy_shunt_na(65535U, &full) == four);

    // The divider's current to within a nanoamp of mV * 1e6 / 660000, and never past the shunt's.
    for (uint32_t mv = 0U; mv <= 6600U; mv++) {
        uint64_t const exact_x660000 = (uint64_t)mv * 1000000U;
        uint32_t const divider = 2000000U - energy_target_na(2000000U, mv);
        assert((uint64_t)divider * 660000U <= exact_x660000 + 660000U &&
               exact_x660000 <= ((uint64_t)divider + 1U) * 660000U);
    }
    assert(energy_target_na(5000U, 3300U) == 0U);
    assert(energy_target_na(0U, 3300U) == 0U);
    assert(energy_target_na(1000000U, 1800U) == 997273U);

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
            energy_total_add(&total, (uint64_t)voltage * current, scale);
            uint64_t const expected = exact >> 32U > UINT64_MAX ? UINT64_MAX : (uint64_t)(exact >> 32U);
            assert(total.pj == expected && total.fraction_q32 == (uint32_t)exact && total.pairs == i + 1U);
        }
    }
}
