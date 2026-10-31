/*
 * Copyright 2017, 2019, 2021 NXP
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Entropy sequence from the MCUXpresso rng_1 driver, with bounded waits.
 */
#include "energy_identity.h"

#include "fsl_clock.h"
#include "fsl_device_registers.h"
#include "fsl_reset.h"

#define ENTROPY_POLLS 1000000U

static bool accumulate(uint32_t* remaining) {
    RNG->COUNTER_CFG = (RNG->COUNTER_CFG & ~RNG_COUNTER_CFG_CLOCK_SEL_MASK) | RNG_COUNTER_CFG_CLOCK_SEL(4U);
    RNG->ONLINE_TEST_CFG = RNG_ONLINE_TEST_CFG_ACTIVATE(1U);
    while (*remaining != 0U) {
        --*remaining;
        uint32_t const statistics = RNG->ONLINE_TEST_VAL;
        uint32_t const min = (statistics & RNG_ONLINE_TEST_VAL_MIN_CHI_SQUARED_MASK) >> RNG_ONLINE_TEST_VAL_MIN_CHI_SQUARED_SHIFT;
        uint32_t const max = (statistics & RNG_ONLINE_TEST_VAL_MAX_CHI_SQUARED_MASK) >> RNG_ONLINE_TEST_VAL_MAX_CHI_SQUARED_SHIFT;
        if (min < max) {
            return true;
        }
    }
    return false;
}

static bool healthy(uint32_t* remaining) {
    for (;;) {
        if (!accumulate(remaining)) {
            return false;
        }
        uint32_t const max = (RNG->ONLINE_TEST_VAL & RNG_ONLINE_TEST_VAL_MAX_CHI_SQUARED_MASK) >> RNG_ONLINE_TEST_VAL_MAX_CHI_SQUARED_SHIFT;
        if (max <= 4U) {
            return true;
        }
        RNG->ONLINE_TEST_CFG = 0U;
        uint32_t const shift = (RNG->COUNTER_CFG & RNG_COUNTER_CFG_SHIFT4X_MASK) >> RNG_COUNTER_CFG_SHIFT4X_SHIFT;
        if (shift < 7U) {
            RNG->COUNTER_CFG = (RNG->COUNTER_CFG & ~RNG_COUNTER_CFG_SHIFT4X_MASK) | RNG_COUNTER_CFG_SHIFT4X(shift + 1U);
        }
    }
}

__attribute__((noinline)) bool energy_identity_boot(uint8_t boot[16]) {
    PMC->PDRUNCFGCLR0 = PMC_PDRUNCFG0_PDEN_RNG_MASK;
    CLOCK_EnableClock(kCLOCK_Rng);
    uint32_t remaining = ENTROPY_POLLS;
    uint32_t const reset_index = (uint32_t)kRNG_RST_SHIFT_RSTn >> 16U;
    uint32_t const reset_mask = 1UL << ((uint32_t)kRNG_RST_SHIFT_RSTn & 0xFFFFU);
    SYSCON->PRESETCTRLSET[reset_index] = reset_mask;
    while (((SYSCON->PRESETCTRLX[reset_index] & reset_mask) == 0U) && (remaining != 0U)) { remaining--; }
    bool ok = remaining != 0U;
    SYSCON->PRESETCTRLCLR[reset_index] = reset_mask;
    while (((SYSCON->PRESETCTRLX[reset_index] & reset_mask) != 0U) && (remaining != 0U)) { remaining--; }
    ok = ok && (remaining != 0U) && healthy(&remaining);
    for (uint32_t word = 0U; ok && (word < 4U); word++) {
        RNG->ONLINE_TEST_CFG = RNG_ONLINE_TEST_CFG_ACTIVATE(1U);
        while (((RNG->COUNTER_VAL & RNG_COUNTER_VAL_REFRESH_CNT_MASK) >> RNG_COUNTER_VAL_REFRESH_CNT_SHIFT) < 31U) {
            if (remaining == 0U) {
                ok = false;
                break;
            }
            remaining--;
        }
        if (ok) {
            uint32_t const value = RNG->RANDOM_NUMBER;
            ok = healthy(&remaining);
            for (uint32_t byte = 0U; byte < 4U; byte++) {
                boot[4U * word + byte] = (uint8_t)(value >> (8U * byte));
            }
        }
    }
    RNG->ONLINE_TEST_CFG = 0U;
    CLOCK_DisableClock(kCLOCK_Rng);
    PMC->PDRUNCFGSET0 = PMC_PDRUNCFG0_PDEN_RNG_MASK;
    return ok;
}
