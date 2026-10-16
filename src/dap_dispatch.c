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
#include "jtag_dp.h"
#include "swd_dp.h"

extern uint32_t arm_DAP_ProcessCommand(const uint8_t* request, uint8_t* response);

uint32_t DAP_ProcessCommand(const uint8_t* request, uint8_t* response) {
    if (*request == ID_DAP_TransferBlock) {
        if (DAP_Data.debug_port == DAP_PORT_SWD) {
            *response = *request;
            return (1U << 16) + 1U + SWD_TransferBlock(request + 1, response + 1);
        }
        // Four wires only. A two-pin block goes to ARM's processing, which reaches the OScan1
        // copies through the dispatcher in src/cjtag_dp.c.
        if ((DAP_Data.debug_port == DAP_PORT_JTAG) && !cJTAG_Active()) {
            *response = *request;
            return (1U << 16) + 1U + JTAG_TransferBlock(request + 1, response + 1);
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
            n = DAP_ProcessCommand(request, response);
            num += n;
            request += (uint16_t)(n >> 16);
            response += (uint16_t)n;
        }
        return (num);
    }

    return DAP_ProcessCommand(request, response);
}
