/*
 * Copyright (c) 2013-2022 ARM Limited. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * From CMSIS-DAP, SW_DP.c's SWD_Transfer and DAP.c's DAP_SWD_TransferBlock. Changed by the
 * Escapement Project: the data phase rewritten, the block loop inlined around the transfer.
 */
/** ARM's SWD transfer, with the data phase written to cost less per bit.
 *
 * ARM's loop spends four operations on each data bit where two will do: it accumulates a running
 * parity, shifts the word down and ors the new bit into the top. The parity is only ever used one
 * bit wide at the end, so it can be taken from the finished word instead of carried; and with the
 * loop unrolled the bit can go straight to its own position, which the barrel shifter folds into
 * the or. Measured on the emulator at the delay-free setting: a read bit goes from 7 cycles to 5
 * and a write bit from 5 to 4, against 3 for a packet bit, which is the floor.
 *
 * Everything else is ARM's, transcribed: the packet phase, the turnaround lengths, the
 * acknowledge handling, the WAIT and FAULT data phases, the protocol-error back-off, the
 * timestamp and the idle cycles. SW_DP.c still supplies SWJ_Sequence and SWD_Sequence, and still
 * compiles unchanged -- CMakeLists.txt renames only its SWD_Transfer out of the way.
 *
 * **The wire must not move.** tools/equiv.py drives this build and a build of ARM's against the
 * same 528 cases and requires identical wire events and identical replies from both. Run it after
 * touching anything here; the point of this file is to be faster, not different.
 */
#include "DAP_config.h"

#include "DAP.h"
#include "hot.h"
#include "swd_dp.h"
#include "swd_transfer.h"

// The same shape as SW_DP.c's, so the two can be read side by side.
#define PIN_SWCLK_SET PIN_SWCLK_TCK_SET
#define PIN_SWCLK_CLR PIN_SWCLK_TCK_CLR

#define SW_CLOCK_CYCLE() \
    PIN_SWCLK_CLR();     \
    PIN_DELAY();         \
    PIN_SWCLK_SET();     \
    PIN_DELAY()

#define SW_WRITE_BIT(bit) \
    PIN_SWDIO_OUT(bit);   \
    PIN_SWCLK_CLR();      \
    PIN_DELAY();          \
    PIN_SWCLK_SET();      \
    PIN_DELAY()

#define SW_READ_BIT(bit)  \
    PIN_SWCLK_CLR();      \
    PIN_DELAY();          \
    bit = PIN_SWDIO_IN(); \
    PIN_SWCLK_SET();      \
    PIN_DELAY()

#define PIN_DELAY() PIN_DELAY_SLOW(DAP_Data.clock_delay)

/** Shift one data bit to its own position in the word, so the or folds the shift in. */
#define SW_READ_DATA_BIT(n) \
    SW_READ_BIT(bit);       \
    val |= bit << (n)

#define SW_WRITE_DATA_BIT(n) SW_WRITE_BIT(val >> (n))

// 0..31, so the two data phases are straight-line code with constant shifts.
#define SW_DATA_BITS(macro) \
    macro(0);               \
    macro(1);               \
    macro(2);               \
    macro(3);               \
    macro(4);               \
    macro(5);               \
    macro(6);               \
    macro(7);               \
    macro(8);               \
    macro(9);               \
    macro(10);              \
    macro(11);              \
    macro(12);              \
    macro(13);              \
    macro(14);              \
    macro(15);              \
    macro(16);              \
    macro(17);              \
    macro(18);              \
    macro(19);              \
    macro(20);              \
    macro(21);              \
    macro(22);              \
    macro(23);              \
    macro(24);              \
    macro(25);              \
    macro(26);              \
    macro(27);              \
    macro(28);              \
    macro(29);              \
    macro(30);              \
    macro(31)

/** Put the packet parity in bit 6, which CMSIS-DAP does not use.
 *
 * ARM sums the four address bits inside the transfer, so a block recomputed the same parity for
 * every word. The caller knows the request before the loop; the transfer then writes the bit with
 * one more shift of a register it is already holding.
 */
__STATIC_FORCEINLINE uint32_t SWD_WithParity(uint32_t request) {
    uint32_t bits = request & 0x0FU;
    bits ^= bits >> 2;
    bits ^= bits >> 1;
    return request | ((bits & 1U) << 6);
}

#define SWD_TransferFunction(speed)                                                                \
    __STATIC_FORCEINLINE uint8_t SWD_Transfer##speed(uint32_t request, uint32_t* data) {           \
        uint32_t ack;                                                                              \
        uint32_t bit;                                                                              \
        uint32_t val;                                                                              \
        uint32_t n;                                                                                \
                                                                                                   \
        /* Packet request. The parity is in bit 6, put there by the caller. */                     \
        SW_WRITE_BIT(1U);           /* Start */                                                    \
        SW_WRITE_BIT(request >> 0); /* APnDP */                                                    \
        SW_WRITE_BIT(request >> 1); /* RnW */                                                      \
        SW_WRITE_BIT(request >> 2); /* A2 */                                                       \
        SW_WRITE_BIT(request >> 3); /* A3 */                                                       \
        SW_WRITE_BIT(request >> 6); /* Parity */                                                   \
        SW_WRITE_BIT(0U);           /* Stop */                                                     \
        SW_WRITE_BIT(1U);           /* Park */                                                     \
                                                                                                   \
        /* Turnaround */                                                                           \
        PIN_SWDIO_OUT_DISABLE();                                                                   \
        for (n = DAP_Data.swd_conf.turnaround; n; n--) {                                           \
            SW_CLOCK_CYCLE();                                                                      \
        }                                                                                          \
                                                                                                   \
        /* Acknowledge */                                                                          \
        SW_READ_BIT(bit);                                                                          \
        ack = bit << 0;                                                                            \
        SW_READ_BIT(bit);                                                                          \
        ack |= bit << 1;                                                                           \
        SW_READ_BIT(bit);                                                                          \
        ack |= bit << 2;                                                                           \
                                                                                                   \
        if (ack == DAP_TRANSFER_OK) {                                                              \
            if (request & DAP_TRANSFER_RnW) {                                                      \
                /* Read data. The parity comes off the finished word rather than a running sum. */ \
                val = 0U;                                                                          \
                SW_DATA_BITS(SW_READ_DATA_BIT);                                                    \
                SW_READ_BIT(bit); /* Parity */                                                     \
                if ((__builtin_parity(val) ^ bit) & 1U) {                                          \
                    ack = DAP_TRANSFER_ERROR;                                                      \
                }                                                                                  \
                if (data) {                                                                        \
                    *data = val;                                                                   \
                }                                                                                  \
                /* Turnaround */                                                                   \
                for (n = DAP_Data.swd_conf.turnaround; n; n--) {                                   \
                    SW_CLOCK_CYCLE();                                                              \
                }                                                                                  \
                PIN_SWDIO_OUT_ENABLE();                                                            \
            } else {                                                                               \
                /* Turnaround */                                                                   \
                for (n = DAP_Data.swd_conf.turnaround; n; n--) {                                   \
                    SW_CLOCK_CYCLE();                                                              \
                }                                                                                  \
                PIN_SWDIO_OUT_ENABLE();                                                            \
                /* Write data */                                                                   \
                val = *data;                                                                       \
                SW_DATA_BITS(SW_WRITE_DATA_BIT);                                                   \
                SW_WRITE_BIT(__builtin_parity(val)); /* Parity */                                  \
            }                                                                                      \
            /* Capture timestamp */                                                                \
            if (request & DAP_TRANSFER_TIMESTAMP) {                                                \
                DAP_Data.timestamp = TIMESTAMP_GET();                                              \
            }                                                                                      \
            /* Idle cycles */                                                                      \
            n = DAP_Data.transfer.idle_cycles;                                                     \
            if (n) {                                                                               \
                PIN_SWDIO_OUT(0U);                                                                 \
                for (; n; n--) {                                                                   \
                    SW_CLOCK_CYCLE();                                                              \
                }                                                                                  \
            }                                                                                      \
            PIN_SWDIO_OUT(1U);                                                                     \
            return ((uint8_t)ack);                                                                 \
        }                                                                                          \
                                                                                                   \
        if ((ack == DAP_TRANSFER_WAIT) || (ack == DAP_TRANSFER_FAULT)) {                           \
            if (DAP_Data.swd_conf.data_phase && ((request & DAP_TRANSFER_RnW) != 0U)) {            \
                for (n = 32U + 1U; n; n--) {                                                       \
                    SW_CLOCK_CYCLE(); /* Dummy read */                                             \
                }                                                                                  \
            }                                                                                      \
            /* Turnaround */                                                                       \
            for (n = DAP_Data.swd_conf.turnaround; n; n--) {                                       \
                SW_CLOCK_CYCLE();                                                                  \
            }                                                                                      \
            PIN_SWDIO_OUT_ENABLE();                                                                \
            if (DAP_Data.swd_conf.data_phase && ((request & DAP_TRANSFER_RnW) == 0U)) {            \
                PIN_SWDIO_OUT(0U);                                                                 \
                for (n = 32U + 1U; n; n--) {                                                       \
                    SW_CLOCK_CYCLE(); /* Dummy write */                                            \
                }                                                                                  \
            }                                                                                      \
            PIN_SWDIO_OUT(1U);                                                                     \
            return ((uint8_t)ack);                                                                 \
        }                                                                                          \
                                                                                                   \
        /* Protocol error: back off the data phase */                                              \
        for (n = DAP_Data.swd_conf.turnaround + 32U + 1U; n; n--) {                                \
            SW_CLOCK_CYCLE();                                                                      \
        }                                                                                          \
        PIN_SWDIO_OUT_ENABLE();                                                                    \
        PIN_SWDIO_OUT(1U);                                                                         \
        return ((uint8_t)ack);                                                                     \
    }

#undef PIN_DELAY
#define PIN_DELAY() PIN_DELAY_FAST()
SWD_TransferFunction(Fast)

#undef PIN_DELAY
#define PIN_DELAY() PIN_DELAY_SLOW(DAP_Data.clock_delay)
    SWD_TransferFunction(Slow)

        uint8_t SWD_Transfer(uint32_t request, uint32_t* data) {
    request = SWD_WithParity(request);
    if (DAP_Data.fast_clock) {
        return SWD_TransferFast(request, data);
    } else {
        return SWD_TransferSlow(request, data);
    }
}

/** DAP_Transfer, expanded once per clock path.
 *
 * ARM's DAP_SWD_Transfer calls SWD_Transfer once per transfer, and SWD_Transfer picks between the
 * two copies on every call. Expanding the command next to each copy lets the transfer inline into
 * it, so a transfer's register prologue is paid once per command rather than once per transfer.
 * The block command is arranged the same way below.
 *
 * This is not the throughput path -- a host reads memory in blocks -- but it is most of what an
 * attach is made of, and of anything touching one register at a time.
 */
#define SWD_XFER_FAST(request, data) SWD_TransferFast(SWD_WithParity(request), data)
#define SWD_XFER_SLOW(request, data) SWD_TransferSlow(SWD_WithParity(request), data)

// clang-format off: each of these expands to a function definition.
// The attribute goes on a declaration because the definitions come out of a macro, which is
// generated from ARM's DAP.c and should stay a faithful copy of it.
static uint32_t SWD_TransferCommandFast(const uint8_t* request, uint8_t* response) HOT_PATH;
static uint32_t SWD_TransferCommandSlow(const uint8_t* request, uint8_t* response) HOT_PATH;
SWD_TRANSFER_FUNCTION(SWD_TransferCommandFast, SWD_XFER_FAST)
SWD_TRANSFER_FUNCTION(SWD_TransferCommandSlow, SWD_XFER_SLOW)
// clang-format on

uint32_t SWD_TransferCommand(const uint8_t* request, uint8_t* response) {
    if (DAP_Data.fast_clock) {
        return SWD_TransferCommandFast(request, response);
    }
    return SWD_TransferCommandSlow(request, response);
}

/** ARM's DAP_SWD_TransferBlock, with the transfer inlined into the loop.
 *
 * A transfer saves eight registers on entry and restores them on exit, and reloads the two GPIO
 * base addresses and the pin masks each time. Called once per word from DAP.c that is about 30
 * cycles of dead wire between one transfer and the next, which at the delay-free setting is a
 * tenth of a read. Inlining it into the loop pays for all of that once per block instead.
 *
 * The logic is ARM's, transcribed. The fast and slow choice is hoisted to the top so each copy
 * inlines one transfer variant rather than both.
 */
#define SWD_TransferBlockFunction(speed)                                                                  \
    static uint32_t SWD_TransferBlock##speed(const uint8_t* request, uint8_t* response) {                 \
        uint32_t request_count;                                                                           \
        uint32_t request_value;                                                                           \
        uint32_t response_count = 0U;                                                                     \
        uint32_t response_value = 0U;                                                                     \
        uint8_t* response_head = response;                                                                \
        uint32_t retry;                                                                                   \
        uint32_t data = 0U;                                                                               \
                                                                                                          \
        response += 3;                                                                                    \
        DAP_TransferAbort = 0U;                                                                           \
        request++; /* Ignore DAP index */                                                                 \
                                                                                                          \
        request_count = (uint32_t)(*(request + 0) << 0) | (uint32_t)(*(request + 1) << 8);                \
        request += 2;                                                                                     \
        if (request_count == 0U) {                                                                        \
            goto end;                                                                                     \
        }                                                                                                 \
                                                                                                          \
        request_value = SWD_WithParity(*request++);                                                       \
        if ((request_value & DAP_TRANSFER_RnW) != 0U) {                                                   \
            if ((request_value & DAP_TRANSFER_APnDP) != 0U) {                                             \
                /* Post AP read */                                                                        \
                retry = DAP_Data.transfer.retry_count;                                                    \
                do {                                                                                      \
                    response_value = SWD_Transfer##speed(request_value, NULL);                            \
                } while ((response_value == DAP_TRANSFER_WAIT) && retry-- && !DAP_TransferAbort);         \
                if (response_value != DAP_TRANSFER_OK) {                                                  \
                    goto read_done;                                                                       \
                }                                                                                         \
            }                                                                                             \
            while (request_count--) {                                                                     \
                if ((request_count == 0U) && ((request_value & DAP_TRANSFER_APnDP) != 0U)) {              \
                    request_value = SWD_WithParity(DP_RDBUFF | DAP_TRANSFER_RnW);                         \
                }                                                                                         \
                retry = DAP_Data.transfer.retry_count;                                                    \
                do {                                                                                      \
                    response_value = SWD_Transfer##speed(request_value, &data);                           \
                } while ((response_value == DAP_TRANSFER_WAIT) && retry-- && !DAP_TransferAbort);         \
                if (response_value != DAP_TRANSFER_OK) {                                                  \
                    goto read_done;                                                                       \
                }                                                                                         \
                /* ARM writes the four bytes separately. GCC merges them back into one store              \
                 * but rebuilds the word from its own bytes first, ten instructions to copy               \
                 * a register, so take the little-endian store directly. */                               \
                __builtin_memcpy(response, &data, sizeof(data));                                          \
                response += sizeof(data);                                                                 \
            }                                                                                             \
        read_done:                                                                                        \
            /* One word is four bytes, so the count the reply carries is the distance the                 \
             * response pointer moved. Keeping a separate counter cost a load, an add and a               \
             * store a word, because the unrolled data phase leaves it nowhere but the stack. */          \
            response_count = (uint32_t)(response - response_head - 3) / 4U;                               \
            goto end;                                                                                     \
        } else {                                                                                          \
            while (request_count--) {                                                                     \
                data = (uint32_t)(*(request + 0) << 0) | (uint32_t)(*(request + 1) << 8) |                \
                       (uint32_t)(*(request + 2) << 16) | (uint32_t)(*(request + 3) << 24);               \
                request += 4;                                                                             \
                retry = DAP_Data.transfer.retry_count;                                                    \
                do {                                                                                      \
                    response_value = SWD_Transfer##speed(request_value, &data);                           \
                } while ((response_value == DAP_TRANSFER_WAIT) && retry-- && !DAP_TransferAbort);         \
                if (response_value != DAP_TRANSFER_OK) {                                                  \
                    goto end;                                                                             \
                }                                                                                         \
                response_count++;                                                                         \
            }                                                                                             \
            /* Check last write */                                                                        \
            retry = DAP_Data.transfer.retry_count;                                                        \
            do {                                                                                          \
                response_value = SWD_Transfer##speed(SWD_WithParity(DP_RDBUFF | DAP_TRANSFER_RnW), NULL); \
            } while ((response_value == DAP_TRANSFER_WAIT) && retry-- && !DAP_TransferAbort);             \
        }                                                                                                 \
                                                                                                          \
    end:                                                                                                  \
        *(response_head + 0) = (uint8_t)(response_count >> 0);                                            \
        *(response_head + 1) = (uint8_t)(response_count >> 8);                                            \
        *(response_head + 2) = (uint8_t)response_value;                                                   \
                                                                                                          \
        return ((uint32_t)(response - response_head));                                                    \
    }

static uint32_t SWD_TransferBlockFast(const uint8_t* request, uint8_t* response) HOT_PATH;
static uint32_t SWD_TransferBlockSlow(const uint8_t* request, uint8_t* response) HOT_PATH;
SWD_TransferBlockFunction(Fast) SWD_TransferBlockFunction(Slow)

    /** The same return shape DAP.c's own DAP_TransferBlock produces. */
    uint32_t SWD_TransferBlock(const uint8_t* request, uint8_t* response) {
    uint32_t num;

    if (DAP_Data.fast_clock) {
        num = SWD_TransferBlockFast(request, response);
    } else {
        num = SWD_TransferBlockSlow(request, response);
    }

    if ((*(request + 3) & DAP_TRANSFER_RnW) != 0U) {
        num |= 4U << 16;
    } else {
        num |= (4U + (((uint32_t)(*(request + 1)) | (uint32_t)(*(request + 2) << 8)) * 4)) << 16;
    }
    return num;
}
