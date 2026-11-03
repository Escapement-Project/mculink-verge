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
#include <stdbool.h>

#include "DAP_config.h"

#include "DAP.h"
#include "cjtag.h"
#include "dap_dispatch.h"
#include "dap_block.h"
#include "dap_packet.h"
#include "hot.h"
#include "jtag_chain.h"
#include "jtag_dp.h"
#include "led.h"
#include "swd_dp.h"

#ifdef SWD_CODEGEN
#include "swd_gen.h"
#include "swd_sweep.h"
#endif
#ifdef JTAG_CODEGEN
#include "jtag_gen.h"
#include "jtag_sweep.h"
#endif

extern uint32_t arm_DAP_ProcessCommand(const uint8_t* request, uint8_t* response);

/** ARM's processing, for every command not routed here. Out of line and in flash: inlined into
 *  the SRAM dispatch below it would take all of DAP.c into SRAM with it.
 *
 * The wire changes only here, in DAP_Connect, DAP_Disconnect and vendor 0x83 (ID_VENDOR_CJTAG_CONTROL),
 * so this is where the FUNC LED learns it. */
__attribute__((noinline)) static uint32_t arm_command(const uint8_t* request, uint8_t* response) {
#ifdef SWD_CODEGEN
    swd_sweep_watch(request);
#endif
#ifdef JTAG_CODEGEN
    jtag_sweep_watch(request);
#endif
    uint32_t const result = arm_DAP_ProcessCommand(request, response);
    if ((*request == ID_DAP_Connect) || (*request == ID_DAP_Disconnect) || (*request == ID_DAP_Vendor3)) {
        if (DAP_Data.debug_port == DAP_PORT_SWD) {
            led_wire(LED_WIRE_SWD);
        } else if (DAP_Data.debug_port == DAP_PORT_JTAG) {
            led_wire(cJTAG_Active() ? LED_WIRE_CJTAG : LED_WIRE_JTAG);
        } else {
            led_wire(LED_WIRE_NONE);
        }
    }
    return result;
}

COMMAND_PATH static uint32_t transfer_block(const uint8_t* request, uint8_t* response) {
    if (DAP_Data.debug_port == DAP_PORT_SWD) {
        return SWD_TransferBlock(request, response);
    }
#ifdef JTAG_CODEGEN
    const uint32_t wire = jtag_gen_wire();
    if (!jtag_gen_ready_transfers(wire)) {
        // Nothing to run it on: no transfers done, and a protocol error, with the wire untouched.
        response[0] = 0U;
        response[1] = 0U;
        response[2] = DAP_TRANSFER_ERROR;
        return dap_block_result(request, 3U);
    }
    const uint32_t length = jgen_TransferBlock(request, response);
    jtag_gen_learn(wire);
    return length;
#else
    if (!cJTAG_Active()) {
        return JTAG_TransferBlock(request, response);
    }
    if (cJTAG_Format() != CJTAG_OSCAN1) {
        return cjtagt_TransferBlock(request, response);
    }
    if (DAP_Data.fast_clock) {
        return cjtagf_TransferBlock(request, response);
    }
    return cjtag_TransferBlock(request, response);
#endif
}

#if defined(SWD_CODEGEN) && defined(JTAG_CODEGEN)
/** Whether the generators can run `hz`. A period longer than either takes, 8 Hz and slower, has
 *  no code, and this build has no ARM path to run it on. */
static bool clock_generates(uint32_t hz) {
    uint32_t const period = ((CPU_CLOCK - 1U) / hz) + 1U;
    return (period <= SWD_GEN_PERIOD_MAX) && (period <= JTAG_GEN_PERIOD_MAX);
}

/** A command whose wire runs on generated code and which has no failure of its own to report.
 *  When nothing could be generated, ARM's handler would answer DAP_OK over a wire it never drove:
 *  these answer DAP_ERROR before it runs. Transfers report the failure themselves. */
static bool generation_refuses(const uint8_t* request) {
    switch (*request) {
    case ID_DAP_JTAG_Sequence:
        return !jtag_gen_ready(jtag_gen_wire());
    case ID_DAP_JTAG_IDCODE:
    case ID_DAP_WriteABORT:
        if ((*request == ID_DAP_WriteABORT) && (DAP_Data.debug_port == DAP_PORT_SWD)) {
            return !swd_gen_ready();
        }
        // ARM's handler refuses an index past the chain itself, without the wire. Otherwise it sets
        // the index and scans IR there, so the code to check is the code for that position.
        if ((DAP_Data.debug_port != DAP_PORT_JTAG) || (request[1] >= DAP_Data.jtag_dev.count)) {
            return false;
        }
        DAP_Data.jtag_dev.index = request[1];
        return !jtag_gen_ready_transfers(jtag_gen_wire());
    default:
        return false;
    }
}

/** `[command, DAP_ERROR]`. execute_one takes the length consumed from its own plan of the request,
 *  so the one returned here is never read. */
static uint32_t refuse(const uint8_t* request, uint8_t* response) {
    response[0] = *request;
    response[1] = DAP_ERROR;
    return (1U << 16) | 2U;
}
#endif

COMMAND_PATH uint32_t DAP_ProcessCommand(const uint8_t* request, uint8_t* response) {
#ifdef JTAG_CODEGEN
    jtag_gen_command = *request;
#endif
    if ((*request == ID_DAP_Delay) && (request[1] == 0U) && (request[2] == 0U)) {
        // ARM's pacing loop decrements before it tests, so a count of zero runs 2^32 times:
        // about ninety seconds with the probe answering nothing and the host waiting. A zero
        // microsecond delay is a legal request, and probe-rs being careful not to send one is
        // the only reason this has never been seen.
        response[0] = *request;
        response[1] = DAP_OK;
        return (3U << 16) | 2U;
    }
#if defined(SWD_CODEGEN) && defined(JTAG_CODEGEN)
    if (generation_refuses(request)) {
        return refuse(request, response);
    }
#endif
    if ((*request == ID_DAP_Transfer) && (DAP_Data.debug_port == DAP_PORT_SWD)) {
#ifdef SWD_CODEGEN
        if (swd_sweep_pending()) {
            return swd_sweep_connect(request, response);
        }
#endif
        *response = *request;
        return (1U << 16) + 1U + SWD_TransferCommand(request + 1, response + 1);
    }
    if ((*request == ID_DAP_Transfer) && (DAP_Data.debug_port == DAP_PORT_JTAG)) {
#ifdef JTAG_CODEGEN
        if (jtag_sweep_pending()) {
            uint32_t const result = jtag_sweep_connect(request, response);
            jtag_gen_learn(jtag_gen_wire());
            return result;
        }
#endif
        *response = *request;
        uint32_t const result = (1U << 16) + 1U + JTAG_TransferCommand(request + 1, response + 1);
#ifdef JTAG_CODEGEN
        jtag_gen_learn(jtag_gen_wire());
#endif
        return result;
    }
    if (*request == ID_DAP_JTAG_Configure) {
        // Validate the whole chain before ARM's handler or either sweep watcher changes it.
        const uint32_t devices = request[1];
        if (!jtag_chain_valid(request + 2U, devices, DAP_JTAG_DEV_CNT)) {
            response[0] = *request;
            response[1] = DAP_ERROR;
            return ((devices + 2U) << 16) | 2U;
        }
    }
    if ((*request == ID_DAP_TransferBlock) &&
        ((DAP_Data.debug_port == DAP_PORT_SWD) || (DAP_Data.debug_port == DAP_PORT_JTAG))) {
        *response = *request;
        return (1U << 16) + 1U + transfer_block(request + 1, response + 1);
    }
#if defined(SWD_CODEGEN) || defined(JTAG_CODEGEN)
    if (*request == ID_DAP_SWJ_Clock) {
        // ARM keeps only the delay it derives from the clock, and the generators want the clock.
        const uint32_t hz = (uint32_t)request[1] | ((uint32_t)request[2] << 8) | ((uint32_t)request[3] << 16) |
                            ((uint32_t)request[4] << 24);
#if defined(SWD_CODEGEN) && defined(JTAG_CODEGEN)
        // ARM's handler refuses 0 Hz the same way, keeping the clock it had.
        if ((hz != 0U) && !clock_generates(hz)) {
            return refuse(request, response);
        }
#endif
        if (hz != 0U) {
#ifdef SWD_CODEGEN
            swd_gen_clock_hz = hz;
#endif
#ifdef JTAG_CODEGEN
            jtag_gen_clock_hz = hz;
#endif
        }
    }
    const uint32_t result = arm_command(request, response);
    // Generate now rather than in the first transfer, which would carry the time it takes; for
    // JTAG the scans alone, since a host often changes the chain or the framing again before it
    // transfers, and the first transfer builds the transfers (jtag_gen_ready_transfers).
    // The two generators share SRAMX, so only the port in use generates.
#ifdef SWD_CODEGEN
    if ((*request == ID_DAP_SWJ_Clock) || (*request == ID_DAP_SWD_Configure) ||
        (*request == ID_DAP_TransferConfigure)) {
        swd_gen_changed();
        if (DAP_Data.debug_port != DAP_PORT_JTAG) {
            (void)swd_gen_ready();
        }
    }
#endif
#ifdef JTAG_CODEGEN
    if (((*request == ID_DAP_SWJ_Clock) || (*request == ID_DAP_JTAG_Configure) ||
         (*request == ID_DAP_TransferConfigure)) &&
        (DAP_Data.debug_port == DAP_PORT_JTAG)) {
        (void)jtag_gen_ready(jtag_gen_wire());
    }
#endif
    return result;
#else
    return arm_command(request, response);
#endif
}

/** The parser owns framing even when a handler exits early on an invalid TAP or transfer fault. */
COMMAND_PATH static uint32_t execute_one(const uint8_t* request, uint32_t length, uint32_t capacity,
                                         uint32_t packet_size, uint8_t* response) {
    dap_packet_plan_t plan;
    if (length == 0U) {
        return 0U;
    }
    if (request[0] == ID_DAP_Info) {
        if (length < 2U) {
            return 0U;
        }
        uint8_t info[257];
        uint32_t const size = arm_DAP_ProcessCommand(request, info) & 0xffffU;
        // ARM's answer is DAP_PACKET_SIZE on every transport. A host sizes its commands and reads
        // by it, so it has to be this transport's: a HID report, or a bulk packet at this speed.
        if ((request[1] == DAP_ID_PACKET_SIZE) && (size == 4U)) {
            info[2] = (uint8_t)packet_size;
            info[3] = (uint8_t)(packet_size >> 8U);
        }
        if (size > capacity) {
            return 0U;
        }
        memcpy(response, info, size);
        return (2U << 16U) | size;
    }
    if (!dap_packet_plan(request, length, capacity, 0U, &plan)) {
        return 0U;
    }
    uint32_t result;
    if (request[0] == ID_DAP_TransferBlock && (request[4] & DAP_TRANSFER_RnW) != 0U) {
        uint8_t const clamped[] = {ID_DAP_TransferBlock, request[1], (uint8_t)plan.limit, (uint8_t)(plan.limit >> 8U),
                                   request[4]};
        result = DAP_ProcessCommand(clamped, response);
    } else if (request[0] == ID_DAP_SWO_Data) {
        uint8_t const clamped[] = {ID_DAP_SWO_Data, (uint8_t)plan.limit, (uint8_t)(plan.limit >> 8U)};
        result = DAP_ProcessCommand(clamped, response);
    } else {
        result = DAP_ProcessCommand(request, response);
    }
    return (plan.consumed << 16U) | (result & 0xffffU);
}

COMMAND_PATH uint32_t DAP_ExecuteCommandSized(const uint8_t* request, uint32_t request_length, uint32_t packet_size,
                                              uint8_t* response) {
    if (request_length == 0U || packet_size == 0U) {
        return 0U;
    }
    if (request_length > DAP_PACKET_SIZE || packet_size > DAP_PACKET_SIZE) {
        response[0] = ID_DAP_Invalid;
        return (request_length << 16U) | 1U;
    }
    if (request[0] != ID_DAP_ExecuteCommands) {
        uint32_t const result = execute_one(request, request_length, packet_size, packet_size, response);
        if (result != 0U) {
            return result;
        }
        if ((request[0] == ID_DAP_Vendor17 || request[0] == ID_DAP_Vendor18) && packet_size >= PRO_BUS_RESPONSE_FIXED) {
            response[0] = request[0];
            response[1] = PRO_BUS_INVALID;
            response[2] = 0U;
            response[3] = 0U;
            return (request_length << 16U) | PRO_BUS_RESPONSE_FIXED;
        }
        response[0] = ID_DAP_Invalid;
        return (request_length << 16U) | 1U;
    }
    if (request_length < 2U || packet_size < 2U) {
        response[0] = ID_DAP_Invalid;
        return (request_length << 16U) | 1U;
    }
    uint32_t input = 2U;
    uint32_t output = 2U;
    response[0] = ID_DAP_ExecuteCommands;
    response[1] = 0U;
    for (uint32_t i = 0U; i < request[1]; i++) {
        uint32_t const result =
            execute_one(request + input, request_length - input, packet_size - output, packet_size, response + output);
        if (result == 0U) {
            break;
        }
        input += result >> 16U;
        output += result & 0xffffU;
        response[1]++;
    }
    return (input << 16U) | output;
}

/** Legacy callers supply a complete packet; production always supplies the received length. */
__attribute__((noinline)) COMMAND_PATH uint32_t DAP_ExecuteCommand(const uint8_t* request, uint8_t* response) {
    return DAP_ExecuteCommandSized(request, DAP_PACKET_SIZE, DAP_PACKET_SIZE, response);
}
