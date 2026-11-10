/* The energy converter's vectors (docs/host-interface.md, "Converting a pair"), written by the
 * firmware's own converter for host converters to check themselves against:
 * tools/energy_vectors.json.
 *
 * Each table gets explicit samples (every point, its neighbours, both ends) and a 64-bit FNV-1a over
 * the whole sweep: the sense current for every code on each range, and the millivolts and divider
 * current for every voltage code. Random pair sequences, with range switches and pauses, give the
 * total each integrates to.
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "energy_pairs.h"
#include "energy_samples.h"

#define FNV_OFFSET UINT64_C(0xcbf29ce484222325)
#define FNV_PRIME UINT64_C(0x100000001b3)

static uint64_t fnv_u32(uint64_t hash, uint32_t value) {
    for (uint32_t i = 0U; i < 4U; i++) {
        hash ^= (value >> (8U * i)) & 0xFFU;
        hash *= FNV_PRIME;
    }
    return hash;
}

typedef struct {
    uint32_t full_scale_mv;
    energy_calibration_t low;
    energy_calibration_t high;
} vector_tables_t;

/** Slopes as the firmware's calibration computes them from the points (src/energy_monitor.c). */
static energy_calibration_t table(uint8_t count, const uint16_t* codes, const uint32_t* currents) {
    energy_calibration_t t = {.count = count};
    for (uint32_t i = 0U; i < count; i++) {
        t.code[i] = codes[i];
        t.current_na[i] = currents[i];
    }
    for (uint32_t i = 0U; (i + 1U) < count; i++) {
        t.slope_na_per_code_q16[i] =
            (uint32_t)(((uint64_t)(currents[i + 1U] - currents[i]) << 16U) / (uint32_t)(codes[i + 1U] - codes[i]));
    }
    return t;
}

static void print_points(const energy_calibration_t* t) {
    printf("[");
    for (uint32_t i = 0U; i < t->count; i++) {
        printf("%s[%u, %" PRIu32 "]", i ? ", " : "", t->code[i], t->current_na[i]);
    }
    printf("]");
}

static uint32_t lcg_state;

static uint32_t lcg(void) {
    lcg_state = lcg_state * 1664525U + 1013904223U;
    return lcg_state >> 8U;
}

static void print_convert(const vector_tables_t* tables, bool last) {
    printf("    {\"full_scale_mv\": %" PRIu32 ", \"low\": ", tables->full_scale_mv);
    print_points(&tables->low);
    printf(", \"high\": ");
    print_points(&tables->high);

    // The sweeps: sense current for every code, each range; millivolts and divider current for every voltage code.
    uint64_t low_hash = FNV_OFFSET;
    uint64_t high_hash = FNV_OFFSET;
    uint64_t voltage_hash = FNV_OFFSET;
    for (uint32_t code = 0U; code <= 0xFFFFU; code++) {
        low_hash = fnv_u32(low_hash, energy_shunt_na((uint16_t)code, &tables->low));
        high_hash = fnv_u32(high_hash, energy_shunt_na((uint16_t)code, &tables->high));
        uint32_t const mv = energy_voltage_mv((uint16_t)code, tables->full_scale_mv);
        uint32_t const divider_na = UINT32_MAX - energy_target_na(UINT32_MAX, mv);
        voltage_hash = fnv_u32(fnv_u32(voltage_hash, mv), divider_na);
    }
    printf(",\n     \"sweep_fnv1a64\": {\"low\": \"%016" PRIx64 "\", \"high\": \"%016" PRIx64 "\", \"voltage\": \"%016" PRIx64
           "\"},\n     \"samples\": [",
           low_hash, high_hash, voltage_hash);

    // Each point, its neighbours and both ends, on each range, with a voltage that varies.
    bool first = true;
    for (uint32_t range = 0U; range < 2U; range++) {
        const energy_calibration_t* const t = range == 0U ? &tables->low : &tables->high;
        uint32_t codes[16];
        uint32_t n = 0U;
        codes[n++] = 0U;
        codes[n++] = 0xFFFFU;
        for (uint32_t i = 0U; i < t->count; i++) {
            for (int32_t d = -1; d <= 1; d++) {
                int32_t const code = (int32_t)t->code[i] + d;
                if ((code >= 0) && (code <= 0xFFFF)) {
                    codes[n++] = (uint32_t)code;
                }
            }
        }
        for (uint32_t i = 0U; i < n; i++) {
            uint16_t const voltage = (uint16_t)(0x4000U + 0x1357U * i);
            uint32_t const mv = energy_voltage_mv(voltage, tables->full_scale_mv);
            uint32_t const shunt = energy_shunt_na((uint16_t)codes[i], t);
            printf("%s[%u, %" PRIu32 ", %u, %" PRIu32 ", %" PRIu32 ", %" PRIu32 "]", first ? "" : ", ", voltage, codes[i],
                   range == 0U ? 1U : 0U, mv, shunt, energy_target_na(shunt, mv));
            first = false;
        }
    }
    printf("]}%s\n", last ? "" : ",");
}

/** A sequence through the firmware's own integration: a pause flushes a waiting pair and, as a
 *  resume does, makes the range before low again (src/energy_monitor.c, energy_capture_start). */
static void print_sequence(const vector_tables_t* tables, uint32_t rate, uint32_t weight_q16, bool between, uint32_t seed,
                           uint32_t length, bool last) {
    static struct energy_integration energy;
    energy = (struct energy_integration){0};
    energy.low_range = tables->low;
    energy.high_range = tables->high;
    energy.full_scale_mv = tables->full_scale_mv;
    energy.scale_q32 = (UINT64_C(1) << 32U) / rate;
    energy.rearm_below_na = UINT32_MAX;
    energy.calibrated = true;
    energy.previous_low = true;
    energy.switch_weight_q16 = weight_q16;
    energy.rearm_between_pairs = between;
    energy_total_t total = {0};
    lcg_state = seed;

    printf("    {\"rate\": %" PRIu32 ", \"full_scale_mv\": %" PRIu32 ", \"switch_weight_q16\": %" PRIu32
           ", \"rearm_between_pairs\": %s, \"low\": ",
           rate, tables->full_scale_mv, weight_q16, between ? "true" : "false");
    print_points(&tables->low);
    printf(", \"high\": ");
    print_points(&tables->high);
    printf(",\n     \"events\": [");
    bool low = true;
    char const* separator = "";
    for (uint32_t i = 0U; i < length; i++) {
        if ((lcg() % 9U) == 0U) {
            low = !low;
        }
        if ((i != 0U) && ((lcg() % 50U) == 0U)) {
            energy_flush_held(&energy, &total);
            energy.previous_low = true;
            energy.previous_known = false;
            printf("%s\"pause\"", separator);
            separator = ", ";
        }
        uint16_t const voltage = (uint16_t)(0x8000U + (lcg() % 0x4000U));
        uint16_t const current = (uint16_t)(lcg() & 0xFFFFU);
        // A capture's three results: command 1's voltage and current, then command 2's range.
        energy_consume_result(&energy, 0x81070000U | voltage, &total);
        energy_consume_result(&energy, 0x81070000U | current, &total);
        energy_consume_result(&energy, 0x82070000U | (low ? 0xFFFFU : 0U), &total);
        printf("%s[%u, %u, %u]", separator, voltage, current, low ? 1U : 0U);
        separator = ", ";
    }
    energy_flush_held(&energy, &total);
    energy_total_fold(&total, energy.scale_q32);
    printf("],\n     \"pj\": %" PRIu64 ", \"fraction_q32\": %" PRIu32 ", \"pairs\": %" PRIu64 "}%s\n", total.pj,
           total.fraction_q32, total.pairs, last ? "" : ",");
}

int main(void) {
    static const uint16_t low3[] = {0x00F2U, 0x2A10U, 0x6B44U};
    static const uint32_t low3_na[] = {4545U, 501000U, 1503000U};
    static const uint16_t high3[] = {0x0310U, 0x3F20U, 0xD5A0U};
    static const uint32_t high3_na[] = {1503000U, 49700000U, 201000000U};
    static const uint16_t low2[] = {0x0100U, 0xF000U};
    static const uint32_t low2_na[] = {4545U, 90000U};
    static const uint16_t high4[] = {0x0400U, 0x2000U, 0x9000U, 0xFF00U};
    static const uint32_t high4_na[] = {90000U, 400000U, 1600000U, 3000000U};
    const vector_tables_t tables[] = {
        {3000U, table(3U, low3, low3_na), table(3U, high3, high3_na)},
        {3000U, table(2U, low2, low2_na), table(4U, high4, high4_na)},
        {1800U, table(3U, low3, low3_na), table(4U, high4, high4_na)},
    };
    uint32_t const count = sizeof(tables) / sizeof(tables[0]);

    printf("{\n  \"format\": 2,\n  \"convert\": [\n");
    for (uint32_t i = 0U; i < count; i++) {
        print_convert(&tables[i], i + 1U == count);
    }
    printf("  ],\n  \"sequences\": [\n");
    // Each rate with a weight its averaging could give, and with and without re-arms between pairs.
    uint32_t const rates[] = {1U, 1000U, 3333U, 100000U, 1000U, 100000U};
    uint32_t const weights[] = {0U, 9786U, 24576U, 15292U, 32768U, 15292U};
    bool const between[] = {true, true, false, true, false, false};
    for (uint32_t i = 0U; i < 6U; i++) {
        print_sequence(&tables[i % count], rates[i], weights[i], between[i], 0x5EED0000U + i, 200U, i == 5U);
    }
    printf("  ]\n}\n");
    return 0;
}
