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
#include "dap_packet.h"
#include "hot.h"
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
    if (jtag_gen_ready_transfers(wire)) {
        const uint32_t length = jgen_TransferBlock(request, response);
        jtag_gen_learn(wire);
        return length;
    }
#endif
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
}

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
    // The single-register command, for the SWD port only. The JTAG and two-pin copies are ARM's
    // because their transfer is reached through a shadowed pin layer, and the win is smaller.
    if ((*request == ID_DAP_Transfer) && (DAP_Data.debug_port == DAP_PORT_SWD)) {
#ifdef SWD_CODEGEN
        if (swd_sweep_pending()) {
            return swd_sweep_connect(request, response);
        }
#endif
        *response = *request;
        return (1U << 16) + 1U + SWD_TransferCommand(request + 1, response + 1);
    }
    if (*request == ID_DAP_JTAG_Configure) {
        // ARM's DAP_JTAG_Configure takes the device count straight from the request and never
        // compares it with DAP_JTAG_DEV_CNT, then writes ir_length[n], ir_before[n] and
        // ir_after[n] for every n below it. A count of 255 against eight-element arrays is a
        // write past all three.
        //
        // **This build does not actually make that write**: the access is undefined once n
        // reaches 8, and GCC uses that to bound the loop, so a 255-device request currently stops
        // inside DAP_Data. That is the compiler's choice and not a property of the code, so it is
        // not something to rely on. Refusing makes it defined.
        //
        // Refusing is also what the protocol already does for a chain this firmware cannot
        // describe, and the request byte count has to match ARM's either way so a
        // DAP_ExecuteCommands batch stays aligned behind it.
        const uint32_t devices = request[1];
        if (devices > DAP_JTAG_DEV_CNT) {
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
        if (hz != 0U) {
#ifdef SWD_CODEGEN
            swd_gen_clock_hz = hz;
#endif
#ifdef JTAG_CODEGEN
            jtag_gen_clock_hz = hz;
#endif
        }
    }
#ifdef JTAG_CODEGEN
    const bool connecting =
        (*request == ID_DAP_Transfer) && (DAP_Data.debug_port == DAP_PORT_JTAG) && jtag_sweep_pending();
    const uint32_t result = connecting ? jtag_sweep_connect(request, response) : arm_command(request, response);
#else
    const uint32_t result = arm_command(request, response);
#endif
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
    if ((*request == ID_DAP_Transfer) && (DAP_Data.debug_port == DAP_PORT_JTAG)) {
        jtag_gen_learn(jtag_gen_wire());
    }
#endif
    return result;
#else
    return arm_command(request, response);
#endif
}

/** The parser owns framing even when a handler exits early on an invalid TAP or transfer fault. */
COMMAND_PATH static uint32_t execute_one(const uint8_t* request, uint32_t length, uint32_t capacity,
                                         uint8_t* response) {
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
        uint32_t const result = execute_one(request, request_length, packet_size, response);
        if (result != 0U) {
            return result;
        }
        if ((request[0] == ID_DAP_Vendor17 || request[0] == ID_DAP_Vendor18) && packet_size >= 4U) {
            response[0] = request[0];
            response[1] = 2U; // PRO_BUS_INVALID
            response[2] = 0U;
            response[3] = 0U;
            return (request_length << 16U) | 4U;
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
            execute_one(request + input, request_length - input, packet_size - output, response + output);
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
