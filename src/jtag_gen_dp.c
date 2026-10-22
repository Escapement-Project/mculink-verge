/*
 * Copyright (c) 2013-2022 ARM Limited. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * From CMSIS-DAP, JTAG_DP.c and DAP.c's JTAG commands. Changed by the Escapement Project: driven
 * over generated clocks, blocks taken whole.
 */
/** ARM's JTAG entry points over the generated clocks, four-wire or two-pin.
 *
 * src/jtag_gen.c generates one function for the wire in use: up to 32 JTAG clocks with TMS held.
 * Everything ARM's JTAG_DP.c does is a run of those, so each entry point here is ARM's clocks, TMS
 * and TDI, in ARM's order, with the timing left to the generated code. Where ARM sets no TDI a
 * clock carries whatever the last one did, and so do these: tools/equiv.py holds them to ARM's
 * clock for clock.
 */
// DAP_config.h first: DAP.h reads the DAP_SWD and DAP_JTAG settings it defines, so a sorted
// include block that puts DAP.h ahead of it will not compile.
#include "DAP_config.h"

#include "DAP.h"
#include "cjtag.h"
#include "hot.h"
#include "jtag_block.h"
#include "jtag_dp.h"
#include "jtag_gen.h"

/** ARM's TDI pin on two pins, where no pin holds it: the level a frame carries when nothing sets
 * it. On four wires the pin itself is the state, as it is for ARM. */
static uint32_t tdi_level;

static uint32_t tdi_now(void) { return cJTAG_Active() ? tdi_level : PIN_TDI_IN(); }

static uint32_t held(void) { return tdi_now() != 0U ? 0xFFFFFFFFU : 0U; }

/** ARM's PIN_TDI_OUT between clocks: on four wires the pin moves now, as ARM's does. */
static void set_tdi(uint32_t level) {
    tdi_level = level;
    if (!cJTAG_Active()) {
        PIN_TDI_OUT(level);
    }
}

/** OScan2's frames: runs in Shift through the scan function, runs outside it through the TMS
 * packets, split where the tracked TAP enters or leaves Shift. Held TMS keeps a Shift run in
 * Shift, so a run leaves it only on its first frame. */
COMMAND_PATH static uint32_t frames_oscan2(uint32_t tdi, uint32_t count, uint32_t tms) {
    uint32_t tdo = 0U;
    for (uint32_t done = 0U; done < count;) {
        uint32_t n = 0U;
        jtag_gen_scan_fn fn = jtag_gen_tms;
        if (cJTAG_TapShift()) {
            fn = jtag_gen_scan;
            n = tms != 0U ? 1U : count - done;
            (void)cJTAG_TapClock(tms);
        } else {
            do {
                (void)cJTAG_TapClock(tms);
                n++;
            } while ((done + n < count) && !cJTAG_TapShift());
        }
        const uint32_t out = fn(tdi >> done, n, tms);
        const uint32_t mask = n < 32U ? (1U << n) - 1U : 0xFFFFFFFFU;
        tdo |= (out & mask) << done;
        done += n;
    }
    return tdo;
}

/** OScan5's frames: runs outside Shift through the scan function, OScan1's three packets; in Shift
 * with TMS 0 through jtag_gen_shift, which stays there; and TMS 1 in Shift through jtag_gen_eot,
 * one frame, which leaves it. */
COMMAND_PATH static uint32_t frames_oscan5(uint32_t tdi, uint32_t count, uint32_t tms) {
    uint32_t tdo = 0U;
    for (uint32_t done = 0U; done < count;) {
        uint32_t n = 0U;
        jtag_gen_scan_fn fn = jtag_gen_scan;
        if (cJTAG_TapShift()) {
            fn = tms != 0U ? jtag_gen_eot : jtag_gen_shift;
            n = tms != 0U ? 1U : count - done;
            (void)cJTAG_TapClock(tms);
        } else {
            do {
                (void)cJTAG_TapClock(tms);
                n++;
            } while ((done + n < count) && !cJTAG_TapShift());
        }
        const uint32_t out = fn(tdi >> done, n, tms);
        const uint32_t mask = n < 32U ? (1U << n) - 1U : 0xFFFFFFFFU;
        tdo |= (out & mask) << done;
        done += n;
    }
    return tdo;
}

/** Up to 32 frames with TMS held and TDI from `tdi`, least significant bit first. */
static uint32_t frames(uint32_t tdi, uint32_t count, uint32_t tms) {
    // jtag_gen_tms exists exactly when the code was generated for OScan2, jtag_gen_eot for OScan5.
    uint32_t tdo;
    if (jtag_gen_tms != NULL) {
        tdo = frames_oscan2(tdi, count, tms);
    } else if (jtag_gen_eot != NULL) {
        tdo = frames_oscan5(tdi, count, tms);
    } else {
        tdo = jtag_gen_scan(tdi, count, tms);
    }
    tdi_level = (tdi >> (count - 1U)) & 1U;
    return tdo;
}

/** Any number of frames with TMS held and TDI where the last frame left it. In SRAM by name: once
 * frames() had OScan5's branch too, gcc stopped inlining this into the SRAM callers. */
COMMAND_PATH static void hold(uint32_t count, uint32_t tms) {
    while (count != 0U) {
        const uint32_t n = count < JTAG_GEN_FRAMES_MAX ? count : JTAG_GEN_FRAMES_MAX;
        (void)frames(held(), n, tms);
        count -= n;
    }
}

/** The data register's last bit, then the bypass bits after it, then Exit1-DR. ARM leaves TDI at
 * the last bit through the bypass. */
COMMAND_PATH static uint32_t last_bit(uint32_t tdi) {
    const uint32_t after = DAP_Data.jtag_dev.count - DAP_Data.jtag_dev.index - 1U;
    if (after == 0U) {
        return frames(tdi, 1U, 1U) & 1U;
    }
    // At most seven, with DAP_JTAG_DEV_CNT devices.
    const uint32_t bit = frames(tdi != 0U ? 0xFFFFFFFFU : 0U, after, 0U) & 1U;
    hold(1U, 1U);
    return bit;
}

void jgen_Sequence(uint32_t info, const uint8_t* tdi, uint8_t* tdo) {
    uint32_t n = info & JTAG_SEQUENCE_TCK;
    if (n == 0U) {
        n = 64U;
    }
    const uint32_t tms = (info & JTAG_SEQUENCE_TMS) != 0U ? 1U : 0U;
    while (n != 0U) {
        const uint32_t count = n < JTAG_GEN_FRAMES_MAX ? n : JTAG_GEN_FRAMES_MAX;
        const uint32_t bytes = (count + 7U) / 8U;
        uint32_t in = 0U;
        for (uint32_t i = 0U; i < bytes; i++) {
            in |= (uint32_t)tdi[i] << (8U * i);
        }
        const uint32_t out = frames(in, count, tms);
        if ((info & JTAG_SEQUENCE_TDO) != 0U) {
            for (uint32_t i = 0U; i < bytes; i++) {
                tdo[i] = (uint8_t)(out >> (8U * i));
            }
            tdo += bytes;
        }
        tdi += bytes;
        n -= count;
    }
}

COMMAND_PATH void jgen_IR(uint32_t ir) {
    const uint32_t index = DAP_Data.jtag_dev.index;
    hold(2U, 1U); // Select-DR-Scan, Select-IR-Scan
    hold(2U, 0U); // Capture-IR, Shift-IR
    set_tdi(1U);
    hold(DAP_Data.jtag_dev.ir_before[index], 0U); // Bypass before data
    // Every IR bit but the last. ARM counts a zero length down from 2^32; this sends none.
    const uint32_t length = DAP_Data.jtag_dev.ir_length[index];
    for (uint32_t n = length != 0U ? length - 1U : 0U; n != 0U;) {
        const uint32_t count = n < JTAG_GEN_FRAMES_MAX ? n : JTAG_GEN_FRAMES_MAX;
        (void)frames(ir, count, 0U);
        ir = count < 32U ? ir >> count : 0U;
        n -= count;
    }
    const uint32_t after = DAP_Data.jtag_dev.ir_after[index];
    if (after != 0U) {
        (void)frames(ir, 1U, 0U); // Set last IR bit
        set_tdi(1U);
        hold(after - 1U, 0U); // Bypass after data
        hold(1U, 1U);         // Bypass & Exit1-IR
    } else {
        (void)frames(ir, 1U, 1U); // Set last IR bit & Exit1-IR
    }
    hold(1U, 1U); // Update-IR
    hold(1U, 0U); // Idle
    set_tdi(1U);
}

uint32_t jgen_ReadIDCode(void) {
    hold(1U, 1U);                             // Select-DR-Scan
    hold(2U + DAP_Data.jtag_dev.index, 0U); // Capture-DR, Shift-DR, bypass before data
    uint32_t val = frames(held(), 31U, 0U);   // D0..D30
    val |= (frames(held(), 1U, 1U) & 1U) << 31; // D31 & Exit1-DR
    hold(1U, 1U);                             // Update-DR
    hold(1U, 0U);                             // Idle
    return val;
}

void jgen_WriteAbort(uint32_t data) {
    hold(1U, 1U);                             // Select-DR-Scan
    hold(2U + DAP_Data.jtag_dev.index, 0U); // Capture-DR, Shift-DR, bypass before data
    set_tdi(0U);
    hold(3U, 0U);                // RnW, A2 and A3, all zero
    (void)frames(data, 31U, 0U); // D0..D30
    (void)last_bit(data >> 31);
    hold(1U, 1U); // Update-DR
    hold(1U, 0U); // Idle
    set_tdi(1U);
}

/** Whether the generated transfers may run: they walk from Run-Test/Idle, and in OScan2 and OScan5
 * a frame's shape depends on where the TAP is, so a host that left it elsewhere gets the scans,
 * which follow the tracked state. */
static bool transfers_fit(void) {
    return (jtag_gen_read != NULL) &&
           (((jtag_gen_tms == NULL) && (jtag_gen_eot == NULL)) || (cJTAG_Tap() == CJTAG_TAP_IDLE));
}

/** One generated transfer, where they exist. */
static uint8_t transfer_one(uint32_t request, uint32_t* data) {
    const jtag_gen_xfer_fn fn = (request & DAP_TRANSFER_RnW) != 0U ? jtag_gen_read : jtag_gen_write;
    uint32_t word = 0U;
    if ((request & DAP_TRANSFER_RnW) == 0U) {
        word = *data;
    }
    const uint32_t ack = (uint32_t)fn(request, (uint8_t*)&word, 1U, tdi_now());
    tdi_level = 1U;
    if (ack == DAP_TRANSFER_WAIT) {
        jtag_gen_waited();
    }
    if ((ack == DAP_TRANSFER_OK) && ((request & DAP_TRANSFER_RnW) != 0U) && (data != NULL)) {
        *data = word;
    }
    if ((request & DAP_TRANSFER_TIMESTAMP) != 0U) {
        DAP_Data.timestamp = TIMESTAMP_GET();
    }
    return (uint8_t)ack;
}

COMMAND_PATH uint8_t jgen_Transfer(uint32_t request, uint32_t* data) {
    if (transfers_fit()) {
        return transfer_one(request, data);
    }
    hold(1U, 1U); // Select-DR-Scan
    // Capture-DR, Shift-DR and the bypass before data with TDI as it was, then RnW, A2 and A3 with
    // the acknowledge coming back: at most twelve frames, with DAP_JTAG_DEV_CNT devices.
    const uint32_t before = 2U + DAP_Data.jtag_dev.index;
    const uint32_t tdi = (tdi_now() != 0U ? (1U << before) - 1U : 0U) | (((request >> 1) & 7U) << before);
    const uint32_t bits = frames(tdi, before + 3U, 0U) >> before;
    const uint32_t ack = ((bits & 1U) << 1) | ((bits >> 1) & 1U) | (((bits >> 2) & 1U) << 2);

    if (ack != DAP_TRANSFER_OK) {
        hold(1U, 1U); // Exit1-DR
    } else if ((request & DAP_TRANSFER_RnW) != 0U) {
        uint32_t val = frames(held(), 31U, 0U); // D0..D30
        val |= last_bit(tdi_now()) << 31;
        if (data != NULL) {
            *data = val;
        }
    } else {
        (void)frames(*data, 31U, 0U); // D0..D30
        (void)last_bit(*data >> 31);
    }

    hold(1U, 1U); // Update-DR
    hold(1U, 0U); // Idle
    set_tdi(1U);
    if ((request & DAP_TRANSFER_TIMESTAMP) != 0U) {
        DAP_Data.timestamp = TIMESTAMP_GET();
    }
    hold(DAP_Data.transfer.idle_cycles, 0U);
    return (uint8_t)ack;
}

// clang-format off: this expands to a function definition.
JTAG_BLOCK_FUNCTION(jgen_BlockImpl, jgen_Transfer, jgen_IR)
// clang-format on

/** `count` transfers of `request`, each retried on WAIT as ARM's block loop retries it. Returns the
 * last acknowledge, and in `done` how many completed. */
COMMAND_PATH static uint32_t transfer_words(uint32_t request, uint8_t* words, uint32_t count, uint32_t* done) {
    const jtag_gen_xfer_fn fn = (request & DAP_TRANSFER_RnW) != 0U ? jtag_gen_read : jtag_gen_write;
    uint32_t ack = DAP_TRANSFER_OK;
    uint32_t n = 0U;
    while (count != 0U) {
        const uint64_t result = fn(request, words, count, tdi_now());
        tdi_level = 1U;
        const uint32_t ok = count - (uint32_t)(result >> 32);
        ack = (uint32_t)result;
        n += ok;
        words += 4U * ok;
        count -= ok;
        if (ack == DAP_TRANSFER_OK) {
            break;
        }
        if (ack == DAP_TRANSFER_WAIT) {
            jtag_gen_waited();
        }
        uint32_t retry = DAP_Data.transfer.retry_count;
        while ((ack == DAP_TRANSFER_WAIT) && retry-- && !DAP_TransferAbort) {
            ack = (uint32_t)fn(request, words, 1U, tdi_now());
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

/** ARM's DAP_JTAG_TransferBlock, a block at a time rather than a word at a time. */
static uint32_t block_whole(const uint8_t* request, uint8_t* response) HOT_PATH;
static uint32_t block_whole(const uint8_t* request, uint8_t* response) {
    uint32_t response_count = 0U;
    uint32_t response_value = 0U;
    uint8_t* response_head = response;
    uint32_t done;
    uint32_t discard;

    response += 3;
    DAP_TransferAbort = 0U;
    DAP_Data.jtag_dev.index = *request++;
    if (DAP_Data.jtag_dev.index >= DAP_Data.jtag_dev.count) {
        goto end;
    }
    const uint32_t request_count = (uint32_t)(*(request + 0) << 0) | (uint32_t)(*(request + 1) << 8);
    request += 2;
    if (request_count == 0U) {
        goto end;
    }
    const uint32_t request_value = *request++;
    // The generated transfers are for one chain position: this command's.
    if (!jtag_gen_ready_transfers(jtag_gen_wire()) || !transfers_fit()) {
        return jgen_BlockImpl(request - 4, response_head);
    }
    const uint32_t ir = (request_value & DAP_TRANSFER_APnDP) != 0U ? JTAG_APACC : JTAG_DPACC;
    jgen_IR(ir);

    if ((request_value & DAP_TRANSFER_RnW) != 0U) {
        // Every JTAG read is posted: the first returns nothing, and the last word comes from RDBUFF.
        response_value = transfer_words(request_value, (uint8_t*)&discard, 1U, &done);
        if (response_value != DAP_TRANSFER_OK) {
            goto read_done;
        }
        response_value = transfer_words(request_value, response, request_count - 1U, &done);
        response += 4U * done;
        if (response_value != DAP_TRANSFER_OK) {
            goto read_done;
        }
        if (ir != JTAG_DPACC) {
            jgen_IR(JTAG_DPACC);
        }
        response_value = transfer_words(DP_RDBUFF | DAP_TRANSFER_RnW, response, 1U, &done);
        response += 4U * done;
    read_done:
        response_count = (uint32_t)(response - response_head - 3) / 4U;
    } else {
        // The generated write only reads from `words`.
        response_value = transfer_words(request_value, (uint8_t*)(uintptr_t)request, request_count, &done);
        response_count = done;
        if (response_value != DAP_TRANSFER_OK) {
            goto end;
        }
        // Check last write
        if (ir != JTAG_DPACC) {
            jgen_IR(JTAG_DPACC);
        }
        response_value = transfer_words(DP_RDBUFF | DAP_TRANSFER_RnW, (uint8_t*)&discard, 1U, &done);
    }

end:
    *(response_head + 0) = (uint8_t)(response_count >> 0);
    *(response_head + 1) = (uint8_t)(response_count >> 8);
    *(response_head + 2) = (uint8_t)response_value;
    return (uint32_t)(response - response_head);
}

/** The same return shape DAP.c's own DAP_TransferBlock produces. */
COMMAND_PATH uint32_t jgen_TransferBlock(const uint8_t* request, uint8_t* response) {
    uint32_t num = block_whole(request, response);

    if ((*(request + 3) & DAP_TRANSFER_RnW) != 0U) {
        num |= 4U << 16;
    } else {
        num |= (4U + (((uint32_t)(*(request + 1)) | (uint32_t)(*(request + 2) << 8)) * 4)) << 16;
    }
    return num;
}
