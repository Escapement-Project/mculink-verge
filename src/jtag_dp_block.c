/*
 * Copyright (c) 2013-2022 ARM Limited. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * From CMSIS-DAP, JTAG_DP.c's JTAG_TransferFunction. Changed by the Escapement Project: inlined
 * into a block loop of its own.
 */
/** ARM's four-wire JTAG transfer, inlined into its own block loop.
 *
 * The transfer itself is ARM's JTAG_TransferFunction transcribed, loops and all. Unrolling its
 * data phase was tried and measured: a read bit went from 6 cycles to 5 as predicted and the
 * whole thing lost 5% to 10%, because the gap between transfers grew by more than the data phase
 * saved. See OPTIMISATIONS.md.
 *
 * What is worth having is the other half of what src/swd_dp.c does: a transfer saves registers on
 * entry, restores them on exit and reloads the GPIO addresses each time, and ARM's block command
 * pays for that once per word. Here the loop and the transfer are in one function.
 *
 * Only the four-wire path. The two-pin copies in src/cjtag_jtag_dp.c are still ARM's source, and
 * a block command reaches them through ARM's own processing.
 *
 * **The wire must not move.** `uv run tools/equiv.py --wire jtag <arm-build> <this-build>` drives
 * both against a modelled TAP over every acknowledge and chain layout.
 */
#include "DAP_config.h"

#include "DAP.h"
#include "dap_block.h"
#include "hot.h"
#include "jtag_block.h"
#include "jtag_dp.h"

// The same shape as JTAG_DP.c's, so the two can be read side by side.
#define PIN_TCK_SET PIN_SWCLK_TCK_SET
#define PIN_TCK_CLR PIN_SWCLK_TCK_CLR
#define PIN_TMS_SET PIN_SWDIO_TMS_SET
#define PIN_TMS_CLR PIN_SWDIO_TMS_CLR

#define JTAG_CYCLE_TCK() \
    PIN_TCK_CLR();       \
    PIN_DELAY();         \
    PIN_TCK_SET();       \
    PIN_DELAY()

#define JTAG_CYCLE_TDI(tdi) \
    PIN_TDI_OUT(tdi);       \
    PIN_TCK_CLR();          \
    PIN_DELAY();            \
    PIN_TCK_SET();          \
    PIN_DELAY()

#define JTAG_CYCLE_TDO(tdo) \
    PIN_TCK_CLR();          \
    PIN_DELAY();            \
    tdo = PIN_TDO_IN();     \
    PIN_TCK_SET();          \
    PIN_DELAY()

#define JTAG_CYCLE_TDIO(tdi, tdo) \
    PIN_TDI_OUT(tdi);             \
    PIN_TCK_CLR();                \
    PIN_DELAY();                  \
    tdo = PIN_TDO_IN();           \
    PIN_TCK_SET();                \
    PIN_DELAY()

#define PIN_DELAY() PIN_DELAY_SLOW(clock_delay)

#define JTAG_TransferFunction(speed) /**/                                                  \
    __STATIC_FORCEINLINE uint8_t jtag4_Transfer##speed(uint32_t request, uint32_t* data) { \
        uint32_t ack;                                                                      \
        uint32_t bit;                                                                      \
        uint32_t val;                                                                      \
        uint32_t n;                                                                        \
        /* Read once; src/swd_dp.c has why naming it is worth two cycles a bit under clang. */ \
        const uint32_t clock_delay = DAP_Data.clock_delay;                                 \
        (void)clock_delay;                                                                 \
                                                                                           \
        PIN_TMS_SET();                                                                     \
        JTAG_CYCLE_TCK(); /* Select-DR-Scan */                                             \
        PIN_TMS_CLR();                                                                     \
        JTAG_CYCLE_TCK(); /* Capture-DR */                                                 \
        JTAG_CYCLE_TCK(); /* Shift-DR */                                                   \
                                                                                           \
        for (n = DAP_Data.jtag_dev.index; n; n--) {                                        \
            JTAG_CYCLE_TCK(); /* Bypass before data */                                     \
        }                                                                                  \
                                                                                           \
        JTAG_CYCLE_TDIO(request >> 1, bit); /* Set RnW, Get ACK.0 */                       \
        ack = bit << 1;                                                                    \
        JTAG_CYCLE_TDIO(request >> 2, bit); /* Set A2,  Get ACK.1 */                       \
        ack |= bit << 0;                                                                   \
        JTAG_CYCLE_TDIO(request >> 3, bit); /* Set A3,  Get ACK.2 */                       \
        ack |= bit << 2;                                                                   \
                                                                                           \
        if (ack != DAP_TRANSFER_OK) {                                                      \
            /* Exit on error */                                                            \
            PIN_TMS_SET();                                                                 \
            JTAG_CYCLE_TCK(); /* Exit1-DR */                                               \
            goto exit;                                                                     \
        }                                                                                  \
                                                                                           \
        if (request & DAP_TRANSFER_RnW) {                                                  \
            /* Read Transfer */                                                            \
            val = 0U;                                                                      \
            for (n = 31U; n; n--) {                                                        \
                JTAG_CYCLE_TDO(bit); /* Get D0..D30 */                                     \
                val |= bit << 31;                                                          \
                val >>= 1;                                                                 \
            }                                                                              \
            n = DAP_Data.jtag_dev.count - DAP_Data.jtag_dev.index - 1U;                    \
            if (n) {                                                                       \
                JTAG_CYCLE_TDO(bit); /* Get D31 */                                         \
                for (--n; n; n--) {                                                        \
                    JTAG_CYCLE_TCK(); /* Bypass after data */                              \
                }                                                                          \
                PIN_TMS_SET();                                                             \
                JTAG_CYCLE_TCK(); /* Bypass & Exit1-DR */                                  \
            } else {                                                                       \
                PIN_TMS_SET();                                                             \
                JTAG_CYCLE_TDO(bit); /* Get D31 & Exit1-DR */                              \
            }                                                                              \
            val |= bit << 31;                                                              \
            if (data) {                                                                    \
                *data = val;                                                               \
            }                                                                              \
        } else {                                                                           \
            /* Write Transfer */                                                           \
            val = *data;                                                                   \
            for (n = 31U; n; n--) {                                                        \
                JTAG_CYCLE_TDI(val); /* Set D0..D30 */                                     \
                val >>= 1;                                                                 \
            }                                                                              \
            n = DAP_Data.jtag_dev.count - DAP_Data.jtag_dev.index - 1U;                    \
            if (n) {                                                                       \
                JTAG_CYCLE_TDI(val); /* Set D31 */                                         \
                for (--n; n; n--) {                                                        \
                    JTAG_CYCLE_TCK(); /* Bypass after data */                              \
                }                                                                          \
                PIN_TMS_SET();                                                             \
                JTAG_CYCLE_TCK(); /* Bypass & Exit1-DR */                                  \
            } else {                                                                       \
                PIN_TMS_SET();                                                             \
                JTAG_CYCLE_TDI(val); /* Set D31 & Exit1-DR */                              \
            }                                                                              \
        }                                                                                  \
                                                                                           \
    exit:                                                                                  \
        JTAG_CYCLE_TCK(); /* Update-DR */                                                  \
        PIN_TMS_CLR();                                                                     \
        JTAG_CYCLE_TCK(); /* Idle */                                                       \
        PIN_TDI_OUT(1U);                                                                   \
                                                                                           \
        /* Capture Timestamp */                                                            \
        if (request & DAP_TRANSFER_TIMESTAMP) {                                            \
            DAP_Data.timestamp = TIMESTAMP_GET();                                          \
        }                                                                                  \
                                                                                           \
        /* Idle cycles */                                                                  \
        n = DAP_Data.transfer.idle_cycles;                                                 \
        while (n--) {                                                                      \
            JTAG_CYCLE_TCK(); /* Idle */                                                   \
        }                                                                                  \
                                                                                           \
        return ((uint8_t)ack);                                                             \
    }

// clang-format off: each of these expands to a function definition, which the formatter reads
// as a declaration and runs into the next one.
#undef PIN_DELAY
#define PIN_DELAY() PIN_DELAY_FAST()
JTAG_TransferFunction(Fast)
JTAG_BLOCK_FUNCTION(JTAG_TransferBlockFast, jtag4_TransferFast, jtag4_IR)

#undef PIN_DELAY
#define PIN_DELAY() PIN_DELAY_SLOW(clock_delay)
JTAG_TransferFunction(Slow)
JTAG_BLOCK_FUNCTION(JTAG_TransferBlockSlow, jtag4_TransferSlow, jtag4_IR)
    // clang-format on

    uint8_t jtag4_Transfer(uint32_t request, uint32_t* data) {
    if (DAP_Data.fast_clock) {
        return jtag4_TransferFast(request, data);
    } else {
        return jtag4_TransferSlow(request, data);
    }
}

/** The same return shape DAP.c's own DAP_TransferBlock produces. */
COMMAND_PATH uint32_t JTAG_TransferBlock(const uint8_t* request, uint8_t* response) {
    uint32_t num;

    if (DAP_Data.fast_clock) {
        num = JTAG_TransferBlockFast(request, response);
    } else {
        num = JTAG_TransferBlockSlow(request, response);
    }

    return dap_block_result(request, num);
}
