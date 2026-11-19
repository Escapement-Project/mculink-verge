/*
 * Copyright (c) 2013-2022 ARM Limited. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * From CMSIS-DAP, DAP.c's DAP_JTAG_TransferBlock. Changed by the Escapement Project: made a macro
 * so the transfer can be inlined.
 */
/** ARM's DAP_JTAG_TransferBlock, as a macro so the transfer can be inlined into the loop.
 *
 * ARM's version calls the transfer through a function pointer's worth of indirection once per
 * word, and a transfer saves registers on entry, restores them on exit and reloads the GPIO
 * addresses each time. Expanding the loop next to a particular transfer lets all of that happen
 * once per block instead.
 *
 * The generated path in src/jtag_gen_dp.c expands it. The logic is ARM's, transcribed.
 */
#ifndef JTAG_BLOCK_H
#define JTAG_BLOCK_H

#include "hot.h"
#include "byte_io.h"

/** ARM's DAP_JTAG_TransferBlock, with the transfer inlined into the loop. A bit path, so it runs
 * from SRAM (src/hot.h). */
#define JTAG_BLOCK_FUNCTION(block_name, xfer, set_ir)                                             \
    HOT_PATH static uint32_t block_name(const uint8_t* request, uint8_t* response) {              \
        uint32_t request_count;                                                                   \
        uint32_t request_value;                                                                   \
        uint32_t response_count = 0U;                                                             \
        uint32_t response_value = 0U;                                                             \
        uint8_t* response_head = response;                                                        \
        uint32_t retry;                                                                           \
        uint32_t data = 0U;                                                                       \
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
        set_ir(ir);                                                                               \
                                                                                                  \
        if ((request_value & DAP_TRANSFER_RnW) != 0U) {                                           \
            /* Post read */                                                                       \
            retry = DAP_Data.transfer.retry_count;                                                \
            do {                                                                                  \
                response_value = xfer(request_value, nullptr);                                      \
            } while ((response_value == DAP_TRANSFER_WAIT) && retry-- && !DAP_TransferAbort);     \
            if (response_value != DAP_TRANSFER_OK) {                                              \
                goto read_done;                                                                   \
            }                                                                                     \
            while (request_count--) {                                                             \
                if (request_count == 0U) {                                                        \
                    if (ir != JTAG_DPACC) {                                                       \
                        set_ir(JTAG_DPACC);                                                       \
                    }                                                                             \
                    request_value = DP_RDBUFF | DAP_TRANSFER_RnW;                                 \
                }                                                                                 \
                retry = DAP_Data.transfer.retry_count;                                            \
                do {                                                                              \
                    response_value = xfer(request_value, &data);                                  \
                } while ((response_value == DAP_TRANSFER_WAIT) && retry-- && !DAP_TransferAbort); \
                if (response_value != DAP_TRANSFER_OK) {                                          \
                    goto read_done;                                                               \
                }                                                                                 \
                /* ARM writes the four bytes separately. GCC merges them back into one store      \
                 * but rebuilds the word from its own bytes first, ten instructions to copy       \
                 * a register, so take the little-endian store directly. */                       \
                __builtin_memcpy(response, &data, sizeof(data));                                  \
                response += sizeof(data);                                                         \
            }                                                                                     \
        read_done:                                                                                \
            /* One word is four bytes, so the count the reply carries is the distance the         \
             * response pointer moved. Keeping a separate counter cost a load, an add and a       \
             * store a word, because the unrolled data phase leaves it nowhere but the stack. */  \
            response_count = (uint32_t)(response - response_head - 3) / 4U;                       \
            goto end;                                                                             \
        } else {                                                                                  \
            while (request_count--) {                                                             \
                data = read_u32_le(request);                                                      \
                request += 4;                                                                     \
                retry = DAP_Data.transfer.retry_count;                                            \
                do {                                                                              \
                    response_value = xfer(request_value, &data);                                  \
                } while ((response_value == DAP_TRANSFER_WAIT) && retry-- && !DAP_TransferAbort); \
                if (response_value != DAP_TRANSFER_OK) {                                          \
                    goto end;                                                                     \
                }                                                                                 \
                response_count++;                                                                 \
            }                                                                                     \
            /* Check last write */                                                                \
            if (ir != JTAG_DPACC) {                                                               \
                set_ir(JTAG_DPACC);                                                               \
            }                                                                                     \
            retry = DAP_Data.transfer.retry_count;                                                \
            do {                                                                                  \
                response_value = xfer(DP_RDBUFF | DAP_TRANSFER_RnW, nullptr);                       \
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

#endif
