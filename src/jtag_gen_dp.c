/*
 * Copyright (c) 2013-2022 ARM Limited. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * From CMSIS-DAP, JTAG_DP.c and DAP.c's JTAG commands. Changed by the Escapement Project: driven
 * over generated clocks, blocks taken whole.
 */
/** ARM's JTAG entry points on two pins, over the generated OScan1 frames.
 *
 * src/jtag_gen.c generates one function: up to 32 frames with TMS held. Everything ARM's
 * JTAG_DP.c does is a run of those, so each entry point here is ARM's frames, TMS and TDI, in
 * ARM's order, with the timing left to the generated code. Where ARM sets no TDI a frame carries
 * whatever the last frame did, and so do these: tools/equiv.py holds them to ARM's frame for frame.
 */
// DAP_config.h first: DAP.h reads the DAP_SWD and DAP_JTAG settings it defines, so a sorted
// include block that puts DAP.h ahead of it will not compile.
#include "DAP_config.h"

#include "DAP.h"
#include "jtag_block.h"
#include "jtag_dp.h"
#include "jtag_gen.h"

/** ARM's TDI pin: the level a frame carries when nothing sets it. */
static uint32_t tdi_level;

static uint32_t held(void) { return tdi_level != 0U ? 0xFFFFFFFFU : 0U; }

/** Up to 32 frames with TMS held and TDI from `tdi`, least significant bit first. */
static uint32_t frames(uint32_t tdi, uint32_t count, uint32_t tms) {
    const uint32_t tdo = jtag_gen_scan(tdi, count, tms);
    tdi_level = (tdi >> (count - 1U)) & 1U;
    return tdo;
}

/** Any number of frames with TMS held and TDI where the last frame left it. */
static void hold(uint32_t count, uint32_t tms) {
    while (count != 0U) {
        const uint32_t n = count < JTAG_GEN_FRAMES_MAX ? count : JTAG_GEN_FRAMES_MAX;
        (void)jtag_gen_scan(held(), n, tms);
        count -= n;
    }
}

/** The data register's last bit, then the bypass bits after it, then Exit1-DR. ARM leaves TDI at
 * the last bit through the bypass. */
static uint32_t last_bit(uint32_t tdi) {
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

void jgen_IR(uint32_t ir) {
    const uint32_t index = DAP_Data.jtag_dev.index;
    hold(2U, 1U); // Select-DR-Scan, Select-IR-Scan
    hold(2U, 0U); // Capture-IR, Shift-IR
    tdi_level = 1U;
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
        tdi_level = 1U;
        hold(after - 1U, 0U); // Bypass after data
        hold(1U, 1U);         // Bypass & Exit1-IR
    } else {
        (void)frames(ir, 1U, 1U); // Set last IR bit & Exit1-IR
    }
    hold(1U, 1U); // Update-IR
    hold(1U, 0U); // Idle
    tdi_level = 1U;
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
    tdi_level = 0U;
    hold(3U, 0U);                // RnW, A2 and A3, all zero
    (void)frames(data, 31U, 0U); // D0..D30
    (void)last_bit(data >> 31);
    hold(1U, 1U); // Update-DR
    hold(1U, 0U); // Idle
    tdi_level = 1U;
}

uint8_t jgen_Transfer(uint32_t request, uint32_t* data) {
    hold(1U, 1U); // Select-DR-Scan
    // Capture-DR, Shift-DR and the bypass before data with TDI as it was, then RnW, A2 and A3 with
    // the acknowledge coming back: at most twelve frames, with DAP_JTAG_DEV_CNT devices.
    const uint32_t before = 2U + DAP_Data.jtag_dev.index;
    const uint32_t tdi = (tdi_level != 0U ? (1U << before) - 1U : 0U) | (((request >> 1) & 7U) << before);
    const uint32_t bits = frames(tdi, before + 3U, 0U) >> before;
    const uint32_t ack = ((bits & 1U) << 1) | ((bits >> 1) & 1U) | (((bits >> 2) & 1U) << 2);

    if (ack != DAP_TRANSFER_OK) {
        hold(1U, 1U); // Exit1-DR
    } else if ((request & DAP_TRANSFER_RnW) != 0U) {
        uint32_t val = frames(held(), 31U, 0U); // D0..D30
        val |= last_bit(tdi_level) << 31;
        if (data != NULL) {
            *data = val;
        }
    } else {
        (void)frames(*data, 31U, 0U); // D0..D30
        (void)last_bit(*data >> 31);
    }

    hold(1U, 1U); // Update-DR
    hold(1U, 0U); // Idle
    tdi_level = 1U;
    if ((request & DAP_TRANSFER_TIMESTAMP) != 0U) {
        DAP_Data.timestamp = TIMESTAMP_GET();
    }
    hold(DAP_Data.transfer.idle_cycles, 0U);
    return (uint8_t)ack;
}

// clang-format off: this expands to a function definition.
JTAG_BLOCK_FUNCTION(jgen_BlockImpl, jgen_Transfer, jgen_IR)
// clang-format on

/** The same return shape DAP.c's own DAP_TransferBlock produces. */
uint32_t jgen_TransferBlock(const uint8_t* request, uint8_t* response) {
    uint32_t num = jgen_BlockImpl(request, response);

    if ((*(request + 3) & DAP_TRANSFER_RnW) != 0U) {
        num |= 4U << 16;
    } else {
        num |= (4U + (((uint32_t)(*(request + 1)) | (uint32_t)(*(request + 2) << 8)) * 4)) << 16;
    }
    return num;
}
