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

#define PIN_DELAY() PIN_DELAY_SLOW(DAP_Data.clock_delay)

#define JTAG_TransferFunction(speed) /**/                                                  \
    __STATIC_FORCEINLINE uint8_t jtag4_Transfer##speed(uint32_t request, uint32_t* data) { \
        uint32_t ack;                                                                      \
        uint32_t bit;                                                                      \
        uint32_t val;                                                                      \
        uint32_t n;                                                                        \
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

/** ARM's DAP_JTAG_TransferBlock, with the transfer inlined into the loop. */
#define JTAG_TransferBlockFunction(speed)                                                         \
    static uint32_t JTAG_TransferBlock##speed(const uint8_t* request, uint8_t* response) {        \
        uint32_t request_count;                                                                   \
        uint32_t request_value;                                                                   \
        uint32_t response_count = 0U;                                                             \
        uint32_t response_value = 0U;                                                             \
        uint8_t* response_head = response;                                                        \
        uint32_t retry;                                                                           \
        uint32_t data;                                                                            \
        uint32_t ir;                                                                              \
                                                                                                  \
        response += 3;                                                                            \
        DAP_TransferAbort = 0U;                                                                   \
                                                                                                  \
        DAP_Data.jtag_dev.index = *request++;                                                     \
        if (DAP_Data.jtag_dev.index >= DAP_Data.jtag_dev.count) {                                 \
            goto end;                                                                             \
        }                                                                                         \
                                                                                                  \
        request_count = (uint32_t)(*(request + 0) << 0) | (uint32_t)(*(request + 1) << 8);        \
        request += 2;                                                                             \
        if (request_count == 0U) {                                                                \
            goto end;                                                                             \
        }                                                                                         \
                                                                                                  \
        request_value = *request++;                                                               \
                                                                                                  \
        /* Select JTAG chain */                                                                   \
        ir = (request_value & DAP_TRANSFER_APnDP) ? JTAG_APACC : JTAG_DPACC;                      \
        jtag4_IR(ir);                                                                             \
                                                                                                  \
        if ((request_value & DAP_TRANSFER_RnW) != 0U) {                                           \
            /* Post read */                                                                       \
            retry = DAP_Data.transfer.retry_count;                                                \
            do {                                                                                  \
                response_value = jtag4_Transfer##speed(request_value, NULL);                      \
            } while ((response_value == DAP_TRANSFER_WAIT) && retry-- && !DAP_TransferAbort);     \
            if (response_value != DAP_TRANSFER_OK) {                                              \
                goto end;                                                                         \
            }                                                                                     \
            while (request_count--) {                                                             \
                if (request_count == 0U) {                                                        \
                    if (ir != JTAG_DPACC) {                                                       \
                        jtag4_IR(JTAG_DPACC);                                                     \
                    }                                                                             \
                    request_value = DP_RDBUFF | DAP_TRANSFER_RnW;                                 \
                }                                                                                 \
                retry = DAP_Data.transfer.retry_count;                                            \
                do {                                                                              \
                    response_value = jtag4_Transfer##speed(request_value, &data);                 \
                } while ((response_value == DAP_TRANSFER_WAIT) && retry-- && !DAP_TransferAbort); \
                if (response_value != DAP_TRANSFER_OK) {                                          \
                    goto end;                                                                     \
                }                                                                                 \
                *response++ = (uint8_t)data;                                                      \
                *response++ = (uint8_t)(data >> 8);                                               \
                *response++ = (uint8_t)(data >> 16);                                              \
                *response++ = (uint8_t)(data >> 24);                                              \
                response_count++;                                                                 \
            }                                                                                     \
        } else {                                                                                  \
            while (request_count--) {                                                             \
                data = (uint32_t)(*(request + 0) << 0) | (uint32_t)(*(request + 1) << 8) |        \
                       (uint32_t)(*(request + 2) << 16) | (uint32_t)(*(request + 3) << 24);       \
                request += 4;                                                                     \
                retry = DAP_Data.transfer.retry_count;                                            \
                do {                                                                              \
                    response_value = jtag4_Transfer##speed(request_value, &data);                 \
                } while ((response_value == DAP_TRANSFER_WAIT) && retry-- && !DAP_TransferAbort); \
                if (response_value != DAP_TRANSFER_OK) {                                          \
                    goto end;                                                                     \
                }                                                                                 \
                response_count++;                                                                 \
            }                                                                                     \
            /* Check last write */                                                                \
            if (ir != JTAG_DPACC) {                                                               \
                jtag4_IR(JTAG_DPACC);                                                             \
            }                                                                                     \
            retry = DAP_Data.transfer.retry_count;                                                \
            do {                                                                                  \
                response_value = jtag4_Transfer##speed(DP_RDBUFF | DAP_TRANSFER_RnW, NULL);       \
            } while ((response_value == DAP_TRANSFER_WAIT) && retry-- && !DAP_TransferAbort);     \
        }                                                                                         \
                                                                                                  \
    end:                                                                                          \
        *(response_head + 0) = (uint8_t)(response_count >> 0);                                    \
        *(response_head + 1) = (uint8_t)(response_count >> 8);                                    \
        *(response_head + 2) = (uint8_t)response_value;                                           \
                                                                                                  \
        return ((uint32_t)(response - response_head));                                            \
    }

#undef PIN_DELAY
#define PIN_DELAY() PIN_DELAY_FAST()
JTAG_TransferFunction(Fast) JTAG_TransferBlockFunction(Fast)

#undef PIN_DELAY
#define PIN_DELAY() PIN_DELAY_SLOW(DAP_Data.clock_delay)
    JTAG_TransferFunction(Slow) JTAG_TransferBlockFunction(Slow)

        uint8_t jtag4_Transfer(uint32_t request, uint32_t* data) {
    if (DAP_Data.fast_clock) {
        return jtag4_TransferFast(request, data);
    } else {
        return jtag4_TransferSlow(request, data);
    }
}

/** The same return shape DAP.c's own DAP_TransferBlock produces. */
uint32_t JTAG_TransferBlock(const uint8_t* request, uint8_t* response) {
    uint32_t num;

    if (DAP_Data.fast_clock) {
        num = JTAG_TransferBlockFast(request, response);
    } else {
        num = JTAG_TransferBlockSlow(request, response);
    }

    if ((*(request + 3) & DAP_TRANSFER_RnW) != 0U) {
        num |= 4U << 16;
    } else {
        num |= (4U + (((uint32_t)(*(request + 1)) | (uint32_t)(*(request + 2) << 8)) * 4)) << 16;
    }
    return num;
}
