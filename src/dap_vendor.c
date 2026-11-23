/** Vendor commands.
 *
 * CMSIS-DAP leaves 0x80..0x9F to the probe. NXP's stock answers 0x80, 0x81 and 0x82, and its 0x82
 * reply has no "CJTAG" magic, which is how a host tells the two images apart (mculink-stock-notes'
 * identity.md has the replies). Neither 0x80 nor 0x81 is implemented here, so any LinkServer
 * feature resting on them will not work against this image.
 */
#include <string.h>

// DAP_config.h first: DAP.h reads the DAP_SWD and DAP_JTAG settings it defines, so a sorted
// include block that puts DAP.h ahead of it will not compile.
#include "DAP_config.h"

#include "DAP.h"
#include "board_pins.h"
#include "byte_io.h"
#include "cjtag.h"
#include "dap_dispatch.h"
#include "dap_shared.h"
#include "dap_vendor.h"
#include "dio.h"
#include "marker.h"
#include "sample_probe.h"
#ifdef CODE_LAB
#include "code_lab.h"
#endif
#ifdef GENERATED_WIRE
#include "jtag_gen.h"
#include "jtag_sweep.h"
#include "swd_sweep.h"
#endif
#include "energy_monitor.h"
#include "pro_bus.h"
#include "target_power.h"
#include "target_sense.h"

/** Answered by ID_VENDOR_CJTAG_INFO, so a host can recognise this firmware rather than assume it.
 *
 * The reply, after the echoed command id:
 *
 *     [0..4]  "CJTAG"                 magic
 *     [5]     format version          8; the fields are format 7's (below)
 *     [6]     capability flags        CJTAG_CAP_*
 *     [7]     build string length n
 *     [8..]   build string, n bytes, not terminated
 *     [8+n]   board                   BOARD_KIND_* (board_straps.h); format 3 on
 *     [9+n]   0, u32                  format 4 to 6: the scan formats; see below
 *     [13+n]  extended capabilities   u32 LE; bit 0: energy capture; bit 2: accumulated energy;
 *                                     bits 3–4: SPI and I2C; bit 5: calibrated energy fields;
 *                                     bit 6: DIO on Verge Aux; bit 7: markers; bit 8: capture
 *                                     identities; bit 9: the range setting; bit 10: DIO streams
 *
 * A host checks the magic to know it is us, the flags to know what it may use, and the build
 * string to know which image it is talking to. The capability flags are what makes this worth
 * having over a version number alone: a feature can arrive without every host learning a new
 * version, and a host can refuse to use something an older image does not have.
 *
 * Reply fields are only ever appended. Capability bits use the existing flags byte and are added
 * in the format that first reports them. probe-rs reads every format as format 2 and ignores what
 * follows the build string, so a field that moved would be misread without a word. A host reads a
 * field only at the format that added it: a v1 HID reply is padded, so a format 2 image has a byte
 * after the build string too.
 *
 * Version 1 had no flags and no build string, and existed only in images that were never flashed.
 *
 * Format 7 replaced vendor 0x83 with ID_VENDOR_JTAG_FORMAT and ID_VENDOR_JTAG_ESCAPE, and format 7
 * is how a host knows the two are there; it finds the formats by trying them. Hosts that
 * know only 0x83 take cJTAG from flag bits 0 and 4 and the formats at [9+n], and none checks the
 * version, so format 7 clears all three: such a host sees no cJTAG and never sends 0x83.
 *
 * Format 8 changes no field: it says ID_VENDOR_JTAG_FORMAT, sent while disconnected, selects the format
 * the next DAP_Connect(2) starts in. Format 7 refused it there.
 */
#define CJTAG_INFO_MAGIC "CJTAG"
#define CJTAG_INFO_VERSION 8U
/// The id, the magic, the version, the flags, the length, the board, the old formats and extended
/// capabilities, around the build string. A v1 HID report holds 64 bytes.
#define CJTAG_INFO_FIXED 18U
static_assert(CJTAG_INFO_FIXED + sizeof(FIRMWARE_VERSION) - 1U <= DAP_SHARED_HID_REPORT_SIZE,
               "the build string pushes vendor 0x82's last fields out of a HID report");

// Bit 0, OScan1 implemented, and bit 4, vendor 0x83 mode 2 taken, are clear from format 7: see
// above. Reuse neither.
/// cJTAG framing is on right now, in whichever format.
#define CJTAG_CAP_OSCAN1_ACTIVE (1U << 1)
/// ID_VENDOR_TARGET is implemented.
#define CJTAG_CAP_TARGET_SENSE (1U << 2)
/// The SWD paths are generated at runtime, and ID_VENDOR_SWD_GEN is implemented.
#define CJTAG_CAP_SWD_CODEGEN (1U << 3)
/// cJTAG frames are generated at runtime, and ID_VENDOR_JTAG_GEN is implemented.
#define CJTAG_CAP_JTAG_CODEGEN (1U << 5)
/// ID_VENDOR_DIO acts: the plain board, and a Pro whose J15 (USB-SIO off) was not fitted when the
/// probe started. The jumpers are read once at boot, so this describes the running image.
#define CJTAG_CAP_DIO (1U << 6)
/// ID_VENDOR_TARGET_POWER is available on an MCU-Link Pro.
#define CJTAG_CAP_TARGET_POWER (1U << 7)
/// Extended capability format 6: vendor command 0x90 controls energy capture.
#define CJTAG_EXT_CAP_ENERGY (1U << 0)
/// ID_VENDOR_ENERGY appends calibrated voltage, current in nA, and range fields. Bit 1 advertised
/// them with the current in uA, so a host that knows only bit 1 reads no current rather than one
/// a thousand times too large.
#define CJTAG_EXT_CAP_ENERGY_CALIBRATED (1U << 5)
/// ID_VENDOR_ENERGY mode 3 reads accumulated energy and the number of integrated pairs.
#define CJTAG_EXT_CAP_ENERGY_ACCUMULATED (1U << 2)
/// ID_VENDOR_SPI is available on the Pro when J15 leaves USB-SIO enabled.
#define CJTAG_EXT_CAP_SPI (1U << 3)
/// ID_VENDOR_I2C is available on the Pro when J15 leaves USB-SIO enabled.
#define CJTAG_EXT_CAP_I2C (1U << 4)
/// Verge Aux carries ID_VENDOR_DIO, set with CJTAG_CAP_DIO. Aux from before it answers [0xff].
#define CJTAG_EXT_CAP_DIO_AUX (1U << 6)
/// ID_VENDOR_MARK and the stream's markers message, set with CJTAG_EXT_CAP_ENERGY.
#define CJTAG_EXT_CAP_MARKS (1U << 7)
#define CJTAG_EXT_CAP_CAPTURE_ID (1U << 8)
/// ID_VENDOR_ENERGY mode 14 sets the ranges a capture may use, and a stream header says which.
#define CJTAG_EXT_CAP_ENERGY_RANGES (1U << 9)
/// 0x90 mode 15 on Verge Aux: a DIO stream, on a board with DIO pins and no energy capture.
#define CJTAG_EXT_CAP_DIO_STREAM (1U << 10)
#ifdef GENERATED_WIRE
#define CJTAG_CAPS_BUILT (CJTAG_CAP_SWD_CODEGEN | CJTAG_CAP_JTAG_CODEGEN)
#else
#define CJTAG_CAPS_BUILT 0U
#endif

static uint32_t info_command(const uint8_t* request, uint8_t* response) {
    (void)request;
    uint8_t* out = response + 1;
    memcpy(out, CJTAG_INFO_MAGIC, sizeof(CJTAG_INFO_MAGIC) - 1U);
    out += sizeof(CJTAG_INFO_MAGIC) - 1U;
    *out++ = (uint8_t)CJTAG_INFO_VERSION;
    *out++ = (uint8_t)(CJTAG_CAP_TARGET_SENSE | CJTAG_CAPS_BUILT |
                       (cJTAG_Active() ? CJTAG_CAP_OSCAN1_ACTIVE : 0U) | (dio_available() ? CJTAG_CAP_DIO : 0U) |
                       (target_power_available() ? CJTAG_CAP_TARGET_POWER : 0U));
    // The same string DAP_Info's product firmware version reports, so a host that knows
    // nothing about this command can still tell two builds apart.
    *out++ = (uint8_t)(sizeof(FIRMWARE_VERSION) - 1U);
    memcpy(out, FIRMWARE_VERSION, sizeof(FIRMWARE_VERSION) - 1U);
    out += sizeof(FIRMWARE_VERSION) - 1U;
    *out++ = board_kind;
    // Format 4 to 6's scan formats, which an older host would take as a reason to frame.
    for (uint32_t i = 0U; i < 4U; i++) {
        *out++ = 0U;
    }
    uint32_t const extended = (dap_shared.energy_available
                                   ? CJTAG_EXT_CAP_ENERGY | CJTAG_EXT_CAP_ENERGY_CALIBRATED |
                                         CJTAG_EXT_CAP_ENERGY_ACCUMULATED | CJTAG_EXT_CAP_MARKS |
                                         CJTAG_EXT_CAP_CAPTURE_ID | CJTAG_EXT_CAP_ENERGY_RANGES
                                   : 0U) |
                              (pro_bus_available() ? CJTAG_EXT_CAP_SPI | CJTAG_EXT_CAP_I2C : 0U) |
                              (dio_available() ? CJTAG_EXT_CAP_DIO_AUX : 0U) |
                              ((dio_available() && !dap_shared.energy_available) ? CJTAG_EXT_CAP_DIO_STREAM : 0U);
    out = put_u32_le(out, extended);
    return (uint32_t)(out - response);
}

static uint32_t jtag_format_command(const uint8_t* request, uint8_t* response) {
    // An on-board MCU-Link has no cJTAG, but takes 0 as every probe that reports the commands does.
    response[1] =
        (((board_kind != BOARD_KIND_ON_BOARD) || (request[1] == CJTAG_SELECT_JTAG)) && cJTAG_Select(request[1]))
            ? DAP_OK
            : DAP_ERROR;
#ifdef GENERATED_WIRE
    // The wire changed: generate for it now rather than in the next command.
    if ((response[1] == DAP_OK) && (DAP_Data.debug_port == DAP_PORT_JTAG)) {
        (void)jtag_gen_ready(jtag_gen_wire());
    }
#endif
    return 2U;
}

static uint32_t jtag_escape_command(const uint8_t* request, uint8_t* response) {
    // Two or more edges from level 0 or 1, on a JTAG connection in any format: the host picks the
    // context.
    if ((board_kind == BOARD_KIND_ON_BOARD) || (DAP_Data.debug_port != DAP_PORT_JTAG) || (request[1] < 2U) ||
        (request[2] > 1U)) {
        response[1] = DAP_ERROR;
    } else {
        cJTAG_Escape(request[1], request[2]);
        response[1] = DAP_OK;
    }
    return 2U;
}

#ifdef MARKER_PINS
static uint32_t j8_command(const uint8_t* request, uint8_t* response) {
    // request[1] names the J8 pins the marker drives; the rest are DIO pins. Zero, as at boot,
    // gives all four to DIO.
    response[1] = dio_marker(request[1]) ? DAP_OK : DAP_ERROR;
    response[2] = (uint8_t)marker_mask;
    return 3U;
}
#endif

/** ID_VENDOR_STAGE takes `mode`: 0 has CPU 1 copy each command and reply itself, 1 has CPU 0 stage
 * them (the default), anything else leaves it. It answers `status, on`. Commands already received
 * keep the way they arrived, so the change reaches the ones after them.
 */
static uint32_t stage_command(const uint8_t* request, uint8_t* response) {
    if (request[1] <= 1U) {
        dap_shared.stage_commands = request[1];
    }
    response[1] = DAP_OK;
    response[2] = dap_shared.stage_commands;
    return 3U;
}

#ifdef DAP_TIMING_DIAGNOSTICS
static uint32_t timing_command(const uint8_t* request, uint8_t* response) {
    (void)request;
    uint32_t const values[] = {
        dap_stage.timing.clock_hz,
        dap_stage.timing.rx_count,
        dap_stage.timing.rx_callback_cycles,
        dap_stage.timing.rx_stage_copy_cycles,
        dap_stage.timing.tx_stage_copy_cycles,
        dap_stage.timing.rx_publish_cycles,
        dap_stage.timing.mailbox1_signal_cycles,
        dap_stage.timing.wake_to_dispatch_cycles,
        dap_stage.timing.dispatch_cycles,
        dap_stage.timing.request_copy_cycles,
        dap_stage.timing.dap_execute_cycles,
        dap_stage.timing.response_copy_cycles,
        dap_stage.timing.reply_publish_cycles,
        dap_stage.timing.tx_count,
        dap_stage.timing.tx_start_cycles,
        dap_stage.timing.tx_in_flight_cycles,
    };
    response[1] = DAP_OK;
    response[2] = 1U;
    memcpy(response + 3U, values, sizeof(values));
    dap_stage.timing.rx_count = 0U;
    dap_stage.timing.rx_callback_cycles = 0U;
    dap_stage.timing.rx_stage_copy_cycles = 0U;
    dap_stage.timing.tx_stage_copy_cycles = 0U;
    dap_stage.timing.rx_publish_cycles = 0U;
    dap_stage.timing.mailbox1_signal_cycles = 0U;
    dap_stage.timing.wake_to_dispatch_cycles = 0U;
    dap_stage.timing.dispatch_cycles = 0U;
    dap_stage.timing.request_copy_cycles = 0U;
    dap_stage.timing.dap_execute_cycles = 0U;
    dap_stage.timing.response_copy_cycles = 0U;
    dap_stage.timing.reply_publish_cycles = 0U;
    dap_stage.timing.tx_count = 0U;
    dap_stage.timing.tx_start_cycles = 0U;
    dap_stage.timing.tx_in_flight_cycles = 0U;
    return 3U + sizeof(values);
}
#endif

bool dap_vendor_on_aux;

/** Each handler answers with its reply's length, the echoed id included. The request's length is
 *  dap_packet_plan's (dap_packet.h): execute_one takes it from there, so no handler states it. */
uint32_t DAP_ProcessVendorCommand(const uint8_t* request, uint8_t* response) {
    // The echoed command id. Every handler keeps it; an id with none replaces it.
    response[0] = request[0];
    switch (request[0]) {
    case ID_VENDOR_CJTAG_INFO:
        return info_command(request, response);
#ifdef MARKER_PINS
    case ID_VENDOR_J8:
        return j8_command(request, response);
#endif
    case ID_VENDOR_TARGET:
        return target_sense_command(request, response);
    case ID_VENDOR_SAMPLE:
        return sample_probe_command(request, response);
#ifdef CODE_LAB
    case ID_VENDOR_LAB:
        return code_lab_command(request, response);
#endif
#ifdef GENERATED_WIRE
    case ID_VENDOR_SWD_GEN:
        return swd_sweep_command(request, response);
    case ID_VENDOR_JTAG_GEN:
        return jtag_sweep_command(request, response);
    case ID_VENDOR_JTAG_PACING:
        return jtag_pacing_command(request, response);
#endif
    case ID_VENDOR_STAGE:
        return stage_command(request, response);
#ifdef GENERATED_WIRE
    case ID_VENDOR_JTAG_LOG:
        return jtag_log_command(request, response);
#endif
    case ID_VENDOR_DIO:
        return dio_command(request, response);
    case ID_VENDOR_TARGET_POWER:
        return target_power_command(request, response);
    case ID_VENDOR_ENERGY:
        return energy_command(request, response);
    case ID_VENDOR_SPI:
        return pro_spi_command(request, response);
    case ID_VENDOR_I2C:
        return pro_i2c_command(request, response);
#ifdef DAP_TIMING_DIAGNOSTICS
    case ID_VENDOR_TIMING:
        return timing_command(request, response);
#endif
    case ID_VENDOR_MARK:
        return energy_mark_command(request, response);
    case ID_VENDOR_JTAG_FORMAT:
        return jtag_format_command(request, response);
    case ID_VENDOR_JTAG_ESCAPE:
        return jtag_escape_command(request, response);
    default:
        // DAP_ProcessCommand hands this return value straight back to the host, so an id we
        // do not answer has to say so itself. A host that got its own id back would read
        // that as success.
        response[0] = ID_DAP_Invalid;
        return 1U;
    }
}
