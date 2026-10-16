/*
 * Copyright (c) 2013-2022 ARM Limited. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * From CMSIS-DAP, DAP.c's DAP_ExecuteCommand and DAP_ProcessCommand. Changed by the Escapement
 * Project: a shorter route for SWD and JTAG transfer blocks.
 */
/** The two command entry points, so a SWD transfer block can take a shorter route.
 *
 * `DAP_SWD_TransferBlock` in ARM's DAP.c calls `SWD_Transfer` once per word, and a transfer saves
 * eight registers on entry, restores them on exit and reloads the GPIO addresses and pin masks
 * each time. At the delay-free setting that is about 30 cycles of dead wire between one transfer
 * and the next, a tenth of a read. `SWD_TransferBlock` in src/swd_dp.c inlines the transfer into
 * its loop and pays for all of that once per block.
 *
 * Everything else goes to ARM's command processing unchanged, which CMakeLists.txt renames out of
 * the way. `DAP_ExecuteCommand` is transcribed because it calls `DAP_ProcessCommand` by name and
 * has to reach ours.
 *
 * `tools/equiv.py` drives this entry point and a build of ARM's against 528 cases and requires
 * identical wire events and identical replies, so the shorter route has to behave like the long
 * one.
 */
#include "DAP_config.h"

#include "DAP.h"
#include "cjtag.h"
#include "dap_dispatch.h"
#include "jtag_dp.h"
#include "swd_dp.h"

extern uint32_t arm_DAP_ProcessCommand(const uint8_t* request, uint8_t* response);

/** Words a block reply fits: the command id, the two count bytes and the status, then the data. */
#define DAP_BLOCK_WORD_LIMIT ((DAP_PACKET_SIZE - 4U) / 4U)

static uint32_t transfer_block(const uint8_t* request, uint8_t* response) {
    if (DAP_Data.debug_port == DAP_PORT_SWD) {
        return SWD_TransferBlock(request, response);
    }
    if (!cJTAG_Active()) {
        return JTAG_TransferBlock(request, response);
    }
    if (DAP_Data.fast_clock) {
        return cjtagf_TransferBlock(request, response);
    }
    return cjtag_TransferBlock(request, response);
}

uint32_t DAP_ProcessCommand(const uint8_t* request, uint8_t* response) {
    if ((*request == ID_DAP_Delay) && (request[1] == 0U) && (request[2] == 0U)) {
        // ARM's pacing loop decrements before it tests, so a count of zero runs 2^32 times:
        // about ninety seconds with the probe answering nothing and the host waiting. A zero
        // microsecond delay is a legal request, and probe-rs being careful not to send one is
        // the only reason this has never been seen.
        response[0] = *request;
        response[1] = DAP_OK;
        return (3U << 16) | 2U;
    }
    // The single-register command, for the SWD port only. The JTAG and two-pin copies are ARM's
    // because their transfer is reached through a shadowed pin layer, and the win is smaller.
    if ((*request == ID_DAP_Transfer) && (DAP_Data.debug_port == DAP_PORT_SWD)) {
        *response = *request;
        return (1U << 16) + 1U + SWD_TransferCommand(request + 1, response + 1);
    }
    if (*request == ID_DAP_TransferBlock) {
        // The word count is sixteen bits and a read block's request is five bytes whatever it
        // says, so a host can ask for 65535 words into a response buffer that holds 127 and the
        // reply runs off the end of SRAM through the stack. A write block carries its data in the
        // request, which the USB read bounds, so only a read can get past the buffer. Clamping
        // reports the shorter count in the reply, which is how the protocol already signals a
        // block that stopped early.
        if ((request[4] & DAP_TRANSFER_RnW) != 0U) {
            uint32_t words = (uint32_t)request[2] | ((uint32_t)request[3] << 8);
            if (words > DAP_BLOCK_WORD_LIMIT) {
                uint8_t clamped[4] = {request[1], (uint8_t)DAP_BLOCK_WORD_LIMIT, (uint8_t)(DAP_BLOCK_WORD_LIMIT >> 8),
                                      request[4]};
                *response = *request;
                return (1U << 16) + 1U + transfer_block(clamped, response + 1);
            }
        }
        if ((DAP_Data.debug_port == DAP_PORT_SWD) || (DAP_Data.debug_port == DAP_PORT_JTAG)) {
            *response = *request;
            return (1U << 16) + 1U + transfer_block(request + 1, response + 1);
        }
    }
    return arm_DAP_ProcessCommand(request, response);
}

/** Kept out of line. It runs once per USB packet, so the call costs nothing, and being a symbol
 * is what lets tools/cycles.py and tools/equiv.py drive the firmware the way main() does. */
__attribute__((noinline)) uint32_t DAP_ExecuteCommand(const uint8_t* request, uint8_t* response) {
    uint32_t cnt;
    uint32_t num;
    uint32_t n;

    if (*request == ID_DAP_ExecuteCommands) {
        *response++ = *request++;
        cnt = *request++;
        *response++ = (uint8_t)cnt;
        num = (2U << 16) | 2U;
        while (cnt--) {
            // Nothing here knows how many bytes actually arrived, so a count of 255 keeps
            // handing sub-commands whatever follows the request buffer, and each one appends to a
            // reply that is bounded by nothing. Only a packet's worth of reply is ever sent, so
            // stopping at a packet costs a conforming host nothing.
            if (((uint16_t)num >= DAP_PACKET_SIZE) || ((uint16_t)(num >> 16) >= DAP_PACKET_SIZE)) {
                break;
            }
            n = DAP_ProcessCommand(request, response);
            num += n;
            request += (uint16_t)(n >> 16);
            response += (uint16_t)n;
        }
        return (num);
    }

    return DAP_ProcessCommand(request, response);
}
