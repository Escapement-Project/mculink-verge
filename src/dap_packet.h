#ifndef DAP_PACKET_H
#define DAP_PACKET_H

#include <stdbool.h>
#include <stdint.h>

#include "DAP.h"
#include "byte_io.h"
#include "dap_vendor.h"
#include "pro_bus.h"

/** One command's request length, its reply's ceiling, and for DAP_TransferBlock's reads and
 *  DAP_SWO_Data the count the reply has room for. */
typedef struct {
    uint32_t consumed;
    uint32_t produced;
    uint32_t limit;
} dap_packet_plan_t;

static inline dap_packet_plan_t dap_lengths(uint32_t in, uint32_t out) { return (dap_packet_plan_t){in, out, 0U}; }

/** The packet ends before its request says how long it is. No packet holds UINT32_MAX bytes, so
 *  dap_packet_plan's length check refuses it. */
static inline dap_packet_plan_t dap_cut_short(void) { return dap_lengths(UINT32_MAX, 0U); }

/** DAP_Transfer: a request byte per transfer, each followed by a value for a write, a value match or
 *  the match mask. A read answers its word unless it is a match, and a timestamp adds a word. */
static inline dap_packet_plan_t dap_transfer_lengths(const uint8_t* p, uint32_t length) {
    if (length < 3U) {
        return dap_cut_short();
    }
    uint32_t in = 3U;
    uint32_t out = 3U;
    for (uint32_t i = 0U; i < p[2]; i++) {
        if (in == length) {
            return dap_cut_short();
        }
        uint32_t const request = p[in++];
        bool const read = (request & DAP_TRANSFER_RnW) != 0U;
        bool const match = (request & (read ? DAP_TRANSFER_MATCH_VALUE : DAP_TRANSFER_MATCH_MASK)) != 0U;
        if (!read || match) {
            if (length - in < 4U) {
                return dap_cut_short();
            }
            in += 4U;
        }
        if (!match) {
            out += read ? 4U : 0U;
            out += (request & DAP_TRANSFER_TIMESTAMP) != 0U ? 4U : 0U;
        }
    }
    return dap_lengths(in, out);
}

/** DAP_TransferBlock: a count and one request byte. Writes carry their words; reads answer as many
 *  as the reply has room for, and execute_one passes ARM that count. */
static inline dap_packet_plan_t dap_transfer_block_lengths(const uint8_t* p, uint32_t length, uint32_t capacity) {
    if (length < 5U || capacity < 4U) {
        return dap_cut_short();
    }
    uint32_t const count = read_u16_le(p + 2U);
    if ((p[4] & DAP_TRANSFER_RnW) == 0U) {
        if (count > (length - 5U) / 4U) {
            return dap_cut_short();
        }
        return dap_lengths(5U + 4U * count, 4U);
    }
    uint32_t const room = (capacity - 4U) / 4U;
    uint32_t const limit = count < room ? count : room;
    return (dap_packet_plan_t){5U, 4U + 4U * limit, limit};
}

/** DAP_SWJ_Sequence: a count of bits, 0 for 256, then the bits. */
static inline dap_packet_plan_t dap_swj_sequence_lengths(const uint8_t* p, uint32_t length) {
    if (length < 2U) {
        return dap_cut_short();
    }
    return dap_lengths(2U + ((p[1] != 0U ? p[1] : 256U) + 7U) / 8U, 2U);
}

static_assert((SWD_SEQUENCE_CLK == JTAG_SEQUENCE_TCK) && (SWD_SEQUENCE_DIN == JTAG_SEQUENCE_TDO),
              "an SWD sequence's info byte is laid out as a JTAG sequence's");

/** DAP_JTAG_Sequence and DAP_SWD_Sequence: a count of sequences, each an info byte, its clocks in the
 *  low six bits (0 for 64), then a bit per clock. A JTAG sequence always carries TDI; an SWD sequence
 *  carries SWDIO only where it drives it. A sequence that captures answers a bit per clock. */
static inline dap_packet_plan_t dap_sequence_lengths(const uint8_t* p, uint32_t length, bool jtag) {
    if (length < 2U) {
        return dap_cut_short();
    }
    uint32_t in = 2U;
    uint32_t out = 2U;
    for (uint32_t i = 0U; i < p[1]; i++) {
        if (in == length) {
            return dap_cut_short();
        }
        uint32_t const info = p[in++];
        uint32_t const clocks = (info & JTAG_SEQUENCE_TCK) != 0U ? info & JTAG_SEQUENCE_TCK : 64U;
        uint32_t const bytes = (clocks + 7U) / 8U;
        bool const capture = (info & JTAG_SEQUENCE_TDO) != 0U;
        if (jtag || !capture) {
            if (bytes > length - in) {
                return dap_cut_short();
            }
            in += bytes;
        }
        if (capture) {
            out += bytes;
        }
    }
    return dap_lengths(in, out);
}

/** DAP_JTAG_Configure: a count of devices, then each one's IR length. */
static inline dap_packet_plan_t dap_jtag_configure_lengths(const uint8_t* p, uint32_t length) {
    if (length < 2U) {
        return dap_cut_short();
    }
    return dap_lengths(2U + p[1], 2U);
}

/** DAP_SWO_Data: the most bytes the host takes, cut to what the reply has room for. */
static inline dap_packet_plan_t dap_swo_data_lengths(const uint8_t* p, uint32_t length, uint32_t capacity) {
    if (length < 3U || capacity < 4U) {
        return dap_cut_short();
    }
    uint32_t const asked = read_u16_le(p + 1U);
    uint32_t const limit = asked < capacity - 4U ? asked : capacity - 4U;
    return (dap_packet_plan_t){3U, 4U + limit, limit};
}

/** DAP_SWO_ExtendedStatus: which of the status, the count and the index and timestamp it answers. */
static inline dap_packet_plan_t dap_swo_extended_status_lengths(const uint8_t* p, uint32_t length) {
    if (length < 2U) {
        return dap_cut_short();
    }
    return dap_lengths(2U, 1U + ((p[1] & 1U) ? 1U : 0U) + ((p[1] & 2U) ? 4U : 0U) + ((p[1] & 4U) ? 8U : 0U));
}

#ifdef CODE_LAB
/** The code lab's operations, by the byte after the id (code_lab.c has their bytes). A write carries
 *  the bytes its count at offset 5 says, and a read answers them. */
static inline dap_packet_plan_t dap_lab_lengths(const uint8_t* p, uint32_t length) {
    if (length < 2U) {
        return dap_cut_short();
    }
    switch (p[1]) {
    case 0U:
        return dap_lengths(2U, 19U);
    case 1U:
    case 2U: {
        if (length < 7U) {
            return dap_cut_short();
        }
        uint32_t const bytes = read_u16_le(p + 5U);
        return p[1] == 1U ? dap_lengths(7U + bytes, 2U) : dap_lengths(7U, 2U + bytes);
    }
    case 3U:
        return dap_lengths(21U, 10U);
    case 4U:
        return dap_lengths(6U, 2U);
    default:
        return dap_lengths(2U, 2U);
    }
}
#endif

/** Energy capture's modes (energy_monitor.c, docs/host-interface.md). */
static inline dap_packet_plan_t dap_energy_lengths(const uint8_t* p, uint32_t length) {
    if (length < 2U) {
        return dap_cut_short();
    }
    uint32_t in;
    switch (p[1]) {
    case 0U:
    case 8U:
        in = 6U;
        break;
    case 4U:
    case 6U:
    case 10U:
    case 14U:
    case 15U:
        in = 3U;
        break;
    case 5U:
    case 9U:
        in = 7U;
        break;
    case 11U:
    case 13U:
        in = 26U;
        break;
    default:
        in = 2U;
        break;
    }
    uint32_t out;
    switch (p[1]) {
    case 0U:
    case 5U:
        out = 7U;
        break;
    case 1U:
        out = 21U;
        break;
    case 2U:
    case 10U:
    case 11U:
    case 13U:
    case 14U:
        out = 3U;
        break;
    case 3U:
        out = 19U;
        break;
    case 4U:
        out = 28U;
        break;
    case 8U:
    case 9U:
        out = 31U;
        break;
    case 12U:
        out = 32U;
        break;
    default:
        out = 2U;
        break;
    }
    return dap_lengths(in, out);
}

/** The Pro's SPI transaction: a fixed head with the byte count at offset 6, then the bytes out, and
 *  as many back. */
static inline dap_packet_plan_t dap_spi_lengths(const uint8_t* p, uint32_t length) {
    if (length < PRO_SPI_REQUEST_FIXED) {
        return dap_cut_short();
    }
    uint32_t const bytes = read_u16_le(p + 6U);
    return dap_lengths(PRO_SPI_REQUEST_FIXED + bytes, PRO_BUS_RESPONSE_FIXED + bytes);
}

/** The Pro's I2C transaction: a fixed head with the bytes written at offset 6 and read at 8. */
static inline dap_packet_plan_t dap_i2c_lengths(const uint8_t* p, uint32_t length) {
    if (length < PRO_I2C_REQUEST_FIXED) {
        return dap_cut_short();
    }
    return dap_lengths(PRO_I2C_REQUEST_FIXED + read_u16_le(p + 6U), PRO_BUS_RESPONSE_FIXED + read_u16_le(p + 8U));
}

/** An energy stream marker: its number, then a label length at offset 5 and the label. */
static inline dap_packet_plan_t dap_mark_lengths(const uint8_t* p, uint32_t length) {
    if (length < 6U) {
        return dap_cut_short();
    }
    return dap_lengths(6U + p[5], 3U);
}

/** Every command's lengths, by id. An id that is no command takes the id alone and answers
 *  ID_DAP_Invalid. */
static inline dap_packet_plan_t dap_command_lengths(const uint8_t* p, uint32_t length, uint32_t capacity,
                                                    uint32_t info_size) {
    switch (p[0]) {
    case ID_DAP_Info:
        return dap_lengths(2U, info_size);
    case ID_DAP_HostStatus:
        return dap_lengths(3U, 2U);
    case ID_DAP_Connect:
        return dap_lengths(2U, 2U);
    case ID_DAP_Disconnect:
        return dap_lengths(1U, 2U);
    case ID_DAP_TransferConfigure:
        return dap_lengths(6U, 2U);
    case ID_DAP_Transfer:
        return dap_transfer_lengths(p, length);
    case ID_DAP_TransferBlock:
        return dap_transfer_block_lengths(p, length, capacity);
    case ID_DAP_WriteABORT:
        return dap_lengths(6U, 2U);
    case ID_DAP_Delay:
        return dap_lengths(3U, 2U);
    case ID_DAP_ResetTarget:
        return dap_lengths(1U, 3U);
    case ID_DAP_SWJ_Pins:
        return dap_lengths(7U, 2U);
    case ID_DAP_SWJ_Clock:
        return dap_lengths(5U, 2U);
    case ID_DAP_SWJ_Sequence:
        return dap_swj_sequence_lengths(p, length);
    case ID_DAP_SWD_Configure:
        return dap_lengths(2U, 2U);
    case ID_DAP_JTAG_Sequence:
        return dap_sequence_lengths(p, length, true);
    case ID_DAP_JTAG_Configure:
        return dap_jtag_configure_lengths(p, length);
    case ID_DAP_JTAG_IDCODE:
        return dap_lengths(2U, 6U);
    case ID_DAP_SWO_Transport:
    case ID_DAP_SWO_Mode:
        return dap_lengths(2U, 2U);
    case ID_DAP_SWO_Baudrate:
        return dap_lengths(5U, 5U);
    case ID_DAP_SWO_Control:
        return dap_lengths(2U, 2U);
    case ID_DAP_SWO_Status:
        return dap_lengths(1U, 6U);
    case ID_DAP_SWO_Data:
        return dap_swo_data_lengths(p, length, capacity);
    case ID_DAP_SWD_Sequence:
        return dap_sequence_lengths(p, length, false);
    case ID_DAP_SWO_ExtendedStatus:
        return dap_swo_extended_status_lengths(p, length);
#ifdef ARM_CJTAG
    // The reference build's port answers DAP_JTAG_Format and DAP_JTAG_Escape at the ids proposed to
    // Arm as well as at 0x95 and 0x96.
    case ID_DAP_JTAG_Format:
        return dap_lengths(2U, 2U);
    case ID_DAP_JTAG_Escape:
        return dap_lengths(3U, 2U);
#endif
    case ID_VENDOR_CJTAG_INFO:
        return dap_lengths(1U, 18U + sizeof(FIRMWARE_VERSION) - 1U);
#ifdef MARKER_PINS
    case ID_VENDOR_J8:
        return dap_lengths(2U, 3U);
#endif
    case ID_VENDOR_TARGET:
        return dap_lengths(3U, 7U);
    case ID_VENDOR_SAMPLE:
        return dap_lengths(10U, 17U);
#ifdef CODE_LAB
    case ID_VENDOR_LAB:
        return dap_lab_lengths(p, length);
#endif
#ifdef GENERATED_WIRE
    case ID_VENDOR_SWD_GEN:
        return dap_lengths(3U, 58U);
    case ID_VENDOR_JTAG_GEN:
        return dap_lengths(4U, 64U);
    case ID_VENDOR_JTAG_PACING:
        return dap_lengths(2U, 4U);
#endif
    case ID_VENDOR_STAGE:
        return dap_lengths(2U, 3U);
#ifdef GENERATED_WIRE
    case ID_VENDOR_JTAG_LOG:
        return dap_lengths(2U, 25U);
#endif
    case ID_VENDOR_DIO:
        // Seven bytes and a group mask each: four on the standalone board (dio.h DIO_GROUPS_MAX).
        return dap_lengths(4U, 11U);
    case ID_VENDOR_TARGET_POWER:
        return dap_lengths(2U, 3U);
    case ID_VENDOR_ENERGY:
        return dap_energy_lengths(p, length);
    case ID_VENDOR_SPI:
        return dap_spi_lengths(p, length);
    case ID_VENDOR_I2C:
        return dap_i2c_lengths(p, length);
    case ID_VENDOR_MARK:
        return dap_mark_lengths(p, length);
    case ID_VENDOR_JTAG_FORMAT:
        return dap_lengths(2U, 2U);
    case ID_VENDOR_JTAG_ESCAPE:
        return dap_lengths(3U, 2U);
    default:
        return dap_lengths(1U, 1U);
    }
}

/** Preflight one command, excluding batches: false when the packet does not hold its request or the
 *  reply would not fit. Info is read-only and sized by its caller. */
static inline bool dap_packet_plan(const uint8_t* p, uint32_t length, uint32_t capacity, uint32_t info_size,
                                   dap_packet_plan_t* plan) {
    if (length == 0U) {
        return false;
    }
    dap_packet_plan_t const lengths = dap_command_lengths(p, length, capacity, info_size);
    if (lengths.consumed > length || lengths.produced > capacity) {
        return false;
    }
    *plan = lengths;
    return true;
}

#endif
