/*
 * Copyright (c) 2016, Freescale Semiconductor, Inc.
 * Copyright 2016-2023 NXP
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The divisor search from the MCUXpresso usart driver's USART_SetBaudRate, which vcom.c's
 * USART_Init runs. Changes: it returns the divisors instead of writing them, checks the line
 * settings first, and computes in 64 bits.
 */
#ifndef VCOM_LINE_H
#define VCOM_LINE_H

#include <stdbool.h>
#include <stdint.h>

/** Match the SDK's preference for oversampling above nine before trying faster rates. */
static inline bool vcom_line_divisors(uint32_t clock, uint32_t baud, uint8_t stop,
                                      uint8_t parity, uint8_t bits, uint32_t* osr, uint32_t* brg) {
    if (clock == 0U || baud == 0U || (stop != 0U && stop != 2U) || parity > 2U ||
        (bits != 7U && bits != 8U)) {
        return false;
    }
    uint32_t best_diff = UINT32_MAX;
    uint32_t best_osr = 0U;
    uint32_t best_brg = 0U;
    uint32_t const allowed = (baud / 100U) * 3U;
    bool found = false;
    for (uint32_t sampling = 16U; sampling >= 5U; sampling--) {
        if (sampling <= 9U && found && best_diff <= allowed) {
            break;
        }
        uint64_t const rounded = (uint64_t)clock * 10U / ((uint64_t)sampling * baud);
        if (rounded < 5U) {
            continue;
        }
        uint64_t const divider = (rounded - 5U) / 10U;
        if (divider > UINT16_MAX) {
            continue;
        }
        uint32_t const actual = clock / (sampling * ((uint32_t)divider + 1U));
        uint32_t const diff = (actual > baud) ? actual - baud : baud - actual;
        if (!found || diff < best_diff) {
            found = true;
            best_diff = diff;
            best_osr = sampling - 1U;
            best_brg = (uint32_t)divider;
        }
    }
    if (!found || best_diff > allowed) {
        return false;
    }
    *osr = best_osr;
    *brg = best_brg;
    return true;
}

#endif
