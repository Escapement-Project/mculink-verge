/*
 * Copyright (c) 2013-2022 ARM Limited. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * From CMSIS-DAP, DAP.c's DAP_SWD_Transfer and DAP_SWD_TransferBlock. Changed by the Escapement
 * Project: run over the generated transfers, a block at a time.
 */
/** ARM's SWD transfer commands over the generated transfers, in product images.
 *
 * The transfers run generated code from SRAMX (src/swd_gen.c), at whatever period the host's clock
 * gives. What stays here is ARM's command logic around it: posted reads, the RDBUFF read that ends
 * a block, and WAIT retries, which the generated loop hands back by stopping at the transfer that
 * did not acknowledge OK.
 *
 * **The wire must not move.** tools/equiv.py drives a product image and the ARM reference build
 * over the same cases and requires identical wire events and identical replies from both.
 */
#include "DAP_config.h"

#include "DAP.h"
#include "dap_block.h"
#include "hot.h"
#include "byte_io.h"
#include "swd_dp.h"
#include "swd_transfer.h"
#include "swd_gen.h"

/** What a transfer runs when nothing could be generated: it leaves the wire alone and fails. There
 * is no ARM path behind the generated one in this build. */
static uint64_t refused_words(uint32_t packet, uint8_t* words, uint32_t count) {
    (void)packet;
    (void)words;
    return DAP_TRANSFER_ERROR | ((uint64_t)count << 32);
}

/** The read and write functions a command runs, chosen once when it starts.
 *
 * Nothing inside one command can change what the generator built for: the clock, the turnaround
 * and the idle cycles each come from a command of their own. Asking `swd_gen_ready` once per
 * transfer cost a call into flash every time. */
struct transfer_fns {
    swd_gen_fn read;
    swd_gen_fn write;
};

static struct transfer_fns transfer_fns(void) {
    if (!swd_gen_ready()) {
        return (struct transfer_fns){refused_words, refused_words};
    }
    return (struct transfer_fns){swd_gen_read, swd_gen_write};
}

static inline swd_gen_fn transfer_fn(const struct transfer_fns* fns, uint32_t request) {
    return ((request & DAP_TRANSFER_RnW) != 0U) ? fns->read : fns->write;
}

/** `count` transfers of `request`, each retried on WAIT as ARM's block loop retries it. Returns
 * the last acknowledge, and in `done` how many transfers completed. */
static uint32_t transfer_words(const struct transfer_fns* fns, uint32_t request, uint8_t* words, uint32_t count,
                               uint32_t* done) HOT_PATH;
static uint32_t transfer_words(const struct transfer_fns* fns, uint32_t request, uint8_t* words, uint32_t count,
                               uint32_t* done) {
    const swd_gen_fn fn = transfer_fn(fns, request);
    const uint32_t packet = swd_gen_packet(request);
    uint32_t ack = DAP_TRANSFER_OK;
    uint32_t n = 0U;
    while (count != 0U) {
        const uint64_t result = fn(packet, words, count);
        const uint32_t ok = count - (uint32_t)(result >> 32);
        ack = (uint32_t)result;
        n += ok;
        words += 4U * ok;
        count -= ok;
        if (ack == DAP_TRANSFER_OK) {
            break;
        }
        uint32_t retry = DAP_Data.transfer.retry_count;
        while ((ack == DAP_TRANSFER_WAIT) && retry-- && !DAP_TransferAbort) {
            ack = (uint32_t)fn(packet, words, 1U);
        }
        if (ack != DAP_TRANSFER_OK) {
            break;
        }
        n++;
        words += 4U;
        count--;
    }
    *done = n;
    return ack;
}

static inline uint8_t transfer_one(const struct transfer_fns* fns, uint32_t request, uint32_t* data) {
    const swd_gen_fn fn = transfer_fn(fns, request);
    uint32_t word = 0U;
    if ((request & DAP_TRANSFER_RnW) == 0U) {
        word = *data;
    }
    const uint32_t ack = (uint32_t)fn(swd_gen_packet(request), (uint8_t*)&word, 1U);
    if (ack == DAP_TRANSFER_OK) {
        if (((request & DAP_TRANSFER_RnW) != 0U) && (data != nullptr)) {
            *data = word;
        }
        if ((request & DAP_TRANSFER_TIMESTAMP) != 0U) {
            DAP_Data.timestamp = TIMESTAMP_GET();
        }
    }
    return (uint8_t)ack;
}

uint8_t SWD_Transfer(uint32_t request, uint32_t* data) {
    const struct transfer_fns fns = transfer_fns();
    return transfer_one(&fns, request, data);
}

/** The functions the running DAP_Transfer resolved, for the per-transfer call ARM's loop makes.
 * The loop comes out of a macro with a fixed signature, so they cannot travel as an argument. */
static struct transfer_fns command_fns;

#define SWD_XFER_GEN(request, data) transfer_one(&command_fns, request, data)

/* DAP_Transfer and DAP_TransferBlock run from SRAM. In flash, at 11 wait states, ARM's command
 * loop and the calls under it cost several microseconds a transfer, more than the wire does. */
// clang-format off: this expands to a function definition.
static uint32_t SWD_TransferCommandGen(const uint8_t* request, uint8_t* response) HOT_PATH;
SWD_TRANSFER_FUNCTION(SWD_TransferCommandGen, SWD_XFER_GEN)
// clang-format on

COMMAND_PATH uint32_t SWD_TransferCommand(const uint8_t* request, uint8_t* response) {
    command_fns = transfer_fns();
    return SWD_TransferCommandGen(request, response);
}

/** ARM's DAP_SWD_TransferBlock, a block at a time rather than a word at a time. */
static uint32_t SWD_TransferBlockGen(const struct transfer_fns* fns, const uint8_t* request, uint8_t* response)
    HOT_PATH;
static uint32_t SWD_TransferBlockGen(const struct transfer_fns* fns, const uint8_t* request, uint8_t* response) {
    uint32_t response_count = 0U;
    uint32_t response_value = 0U;
    uint8_t* response_head = response;
    uint32_t done;
    uint32_t discard;

    response += 3;
    DAP_TransferAbort = 0U;
    request++; /* Ignore DAP index */

    const uint32_t request_count = (uint32_t)(*(request + 0) << 0) | (uint32_t)(*(request + 1) << 8);
    request += 2;
    if (request_count == 0U) {
        goto end;
    }

    const uint32_t request_value = *request++;
    if ((request_value & DAP_TRANSFER_RnW) != 0U) {
        if ((request_value & DAP_TRANSFER_APnDP) != 0U) {
            /* Post AP read */
            response_value = transfer_words(fns, request_value, (uint8_t*)&discard, 1U, &done);
            if (response_value != DAP_TRANSFER_OK) {
                goto read_done;
            }
        }
        response_value = transfer_words(fns, request_value, response, request_count - 1U, &done);
        response += 4U * done;
        if (response_value != DAP_TRANSFER_OK) {
            goto read_done;
        }
        /* A block posted to an access port reads its last word out of RDBUFF. */
        const uint32_t last =
            ((request_value & DAP_TRANSFER_APnDP) != 0U) ? (DP_RDBUFF | DAP_TRANSFER_RnW) : request_value;
        response_value = transfer_words(fns, last, response, 1U, &done);
        response += 4U * done;
    read_done:
        response_count = (uint32_t)(response - response_head - 3) / 4U;
    } else {
        // The generated write only reads from `words`.
        response_value = transfer_words(fns, request_value, (uint8_t*)(uintptr_t)request, request_count, &done);
        response_count = done;
        if (response_value != DAP_TRANSFER_OK) {
            goto end;
        }
        /* Check last write */
        response_value = transfer_words(fns, DP_RDBUFF | DAP_TRANSFER_RnW, (uint8_t*)&discard, 1U, &done);
    }

end:
    *(response_head + 0) = (uint8_t)(response_count >> 0);
    *(response_head + 1) = (uint8_t)(response_count >> 8);
    *(response_head + 2) = (uint8_t)response_value;

    return ((uint32_t)(response - response_head));
}

/** The same return shape DAP.c's own DAP_TransferBlock produces. */
COMMAND_PATH uint32_t SWD_TransferBlock(const uint8_t* request, uint8_t* response) {
    const struct transfer_fns fns = transfer_fns();
    uint32_t num = SWD_TransferBlockGen(&fns, request, response);

    return dap_block_result(request, num);
}
