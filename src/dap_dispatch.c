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

/** Words a block reply fits: the command id, the two count bytes and the status, then the data. */
#define DAP_BLOCK_WORD_LIMIT ((DAP_PACKET_SIZE - 4U) / 4U)

/** Does a sequence list stay inside the packet the host actually sent?
 *
 * `DAP_SWD_Sequence` and `DAP_JTAG_Sequence` take a sequence count of up to 255 and walk the
 * request forward by one info byte plus up to eight data bytes each, so 255 sequences read about
 * 2 KB from a 512-byte buffer. Nothing in ARM's command layer knows how long the request was:
 * `DAP_ExecuteCommand` is handed a pointer and nothing else, so every handler trusts its own
 * count. The bytes past the end are whatever follows the buffer, and a sequence clocks them onto
 * the wire.
 *
 * Walking the list the same way the handler will is the only way to answer this, because a
 * sequence's length is in its own info byte. `swd` picks the one difference between the two: an
 * SWD input sequence carries no data in the request, where JTAG always supplies TDI.
 */
static bool sequence_fits(const uint8_t* request, bool swd) {
    uint32_t remaining = request[1];
    uint32_t used = 2U; // the command id and the sequence count

    while (remaining--) {
        if (used >= DAP_PACKET_SIZE) {
            return false;
        }
        const uint32_t info = request[used];
        uint32_t clocks = info & 0x3FU; // SWD_SEQUENCE_CLK and JTAG_SEQUENCE_TCK are both this
        if (clocks == 0U) {
            clocks = 64U;
        }
        used += 1U;
        if (!(swd && ((info & SWD_SEQUENCE_DIN) != 0U))) {
            used += (clocks + 7U) / 8U;
        }
        if (used > DAP_PACKET_SIZE) {
            return false;
        }
    }
    return true;
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
        return cjtag2_TransferBlock(request, response);
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
    if ((*request == ID_DAP_SWD_Sequence) || (*request == ID_DAP_JTAG_Sequence)) {
        if (!sequence_fits(request, *request == ID_DAP_SWD_Sequence)) {
            // There is no way to say how much the host meant to send, so the rest of the packet
            // goes with it: a batch behind a malformed command cannot be trusted to be aligned.
            response[0] = *request;
            response[1] = DAP_ERROR;
            return (DAP_PACKET_SIZE << 16) | 2U;
        }
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

/** Kept out of line. It runs once per USB packet, so the call costs nothing, and being a symbol
 * is what lets tools/cycles.py and tools/equiv.py drive the firmware the way main() does. */
__attribute__((noinline)) COMMAND_PATH uint32_t DAP_ExecuteCommand(const uint8_t* request, uint8_t* response) {
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
