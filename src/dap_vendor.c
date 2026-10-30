/** Vendor commands.
 *
 * CMSIS-DAP leaves 0x80..0x9F to the probe. NXP's V3.172 answers 0x80 and 0x81, and echoes 0x82
 * with nothing after it, which is how a host tells the two images apart (mculink-stock-notes'
 * identity.md has the replies). Neither 0x80 nor 0x81 is implemented here, so any LinkServer
 * feature resting on them will not work against this image.
 */
#include <string.h>

// DAP_config.h first: DAP.h reads the DAP_SWD and DAP_JTAG settings it defines, so a sorted
// include block that puts DAP.h ahead of it will not compile.
#include "DAP_config.h"

#include "DAP.h"
#include "board_pins.h"
#include "cjtag.h"
#include "dap_dispatch.h"
#include "dap_shared.h"
#include "dio.h"
#include "marker.h"
#include "reboot.h"
#include "sample_probe.h"
#ifdef CODE_LAB
#include "code_lab.h"
#endif
#ifdef SWD_CODEGEN
#include "swd_gen.h"
#include "swd_sweep.h"
#endif
#ifdef JTAG_CODEGEN
#include "jtag_gen.h"
#include "jtag_sweep.h"
#endif
#include "target_sense.h"
#include "target_power.h"
#include "energy_monitor.h"
#include "pro_bus.h"

/// Identifies this firmware and says what it can do. Read-only, so a host may send it blindly.
#define ID_VENDOR_CJTAG_INFO ID_DAP_Vendor2
/// Turns OScan1 framing on and off.
#define ID_VENDOR_CJTAG_CONTROL ID_DAP_Vendor3
/// Hands J8's four pins to the host, so a pad can be identified and a trigger can be placed.
#define ID_VENDOR_J8 ID_DAP_Vendor4
/// Resets the probe, optionally into the ROM bootloader, so a reflash costs no hands.
#define ID_VENDOR_REBOOT ID_DAP_Vendor5
/// Reads the target's supply and J6 pin 9's detect line. See TARGET_* below.
#define ID_VENDOR_TARGET ID_DAP_Vendor6
/// DPIDR reads with the SWDIO sample placed by the host. An instrument; see sample_probe.c.
#define ID_VENDOR_SAMPLE ID_DAP_Vendor7
/// Runs host-supplied code from RAM, masked. An instrument, built only with CODE_LAB; see code_lab.c.
#define ID_VENDOR_LAB ID_DAP_Vendor8
/// Reports the generated SWD paths and sets the read latency they are generated for. See below.
#define ID_VENDOR_SWD_GEN ID_DAP_Vendor9
/// The same for the generated two-pin frames.
#define ID_VENDOR_JTAG_GEN ID_DAP_Vendor10
/// Switches the generated JTAG transfers' learned pacing. See below.
#define ID_VENDOR_JTAG_PACING ID_DAP_Vendor11
/// Switches whether CPU 0 stages commands for CPU 1 (dap_stage in dap_shared.h). See below.
#define ID_VENDOR_STAGE ID_DAP_Vendor12
/// Reads back the JTAG generator's log of what each generation was for. See below.
#define ID_VENDOR_JTAG_LOG ID_DAP_Vendor13
/// Drives and reads the Pro's DIO pins on J19 (dio.h). See below.
#define ID_VENDOR_DIO ID_DAP_Vendor14
/// Switches and reads the Pro's target supply (target_power.h).
#define ID_VENDOR_TARGET_POWER ID_DAP_Vendor15
/// Starts and stops voltage/current capture on the Pro.
#define ID_VENDOR_ENERGY ID_DAP_Vendor16
/// Executes one full-duplex transaction on the Pro's hardware SPI pins.
#define ID_VENDOR_SPI ID_DAP_Vendor17
/// Executes one Pro I2C write, read, or combined transaction.
#define ID_VENDOR_I2C ID_DAP_Vendor18
#define ID_VENDOR_TIMING ID_DAP_Vendor19
/// Marks a moment in Verge Aux's energy stream, stamped as the command runs (HOST-INTERFACE.md,
/// "Markers: 0x94").
#define ID_VENDOR_MARK ID_DAP_Vendor20

/** Answered by ID_VENDOR_CJTAG_INFO, so a host can recognise this firmware rather than assume it.
 *
 * The reply, after the echoed command id:
 *
 *     [0..4]  "CJTAG"                 magic
 *     [5]     format version          this layout is 6
 *     [6]     capability flags        CJTAG_CAP_*
 *     [7]     build string length n
 *     [8..]   build string, n bytes, not terminated
 *     [8+n]   board                   BOARD_KIND_* (board_pins.h); format 3 on
 *     [9+n]   scan formats, u32 LE    bit n: SCNFMT n offered (cJTAG_Offered); format 4 on
 *     [13+n]  extended capabilities   u32 LE; bit 0: energy capture; bit 2: accumulated energy;
 *                                     bits 3–4: SPI and I2C; bit 5: calibrated energy fields;
 *                                     bit 6: DIO on Verge Aux; bit 7: markers
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
 */
#define CJTAG_INFO_MAGIC "CJTAG"
#define CJTAG_INFO_VERSION 6U
/// The id, the magic, the version, the flags, the length, the board, scan formats and extended
/// capabilities, around the build string. A v1 HID report holds 64 bytes.
#define CJTAG_INFO_FIXED 18U
static_assert(CJTAG_INFO_FIXED + sizeof(FIRMWARE_VERSION) - 1U <= DAP_SHARED_HID_REPORT_SIZE,
               "the build string pushes vendor 0x82's last fields out of a HID report");

/// Two-pin OScan1 JTAG is implemented. Cleared on an on-board MCU-Link, where nobody has framed
/// OScan1 through the buffers yet.
#define CJTAG_CAP_OSCAN1 (1U << 0)
/// Two-pin framing is on right now, in whichever format: ID_VENDOR_CJTAG_CONTROL's state.
#define CJTAG_CAP_OSCAN1_ACTIVE (1U << 1)
/// ID_VENDOR_TARGET is implemented.
#define CJTAG_CAP_TARGET_SENSE (1U << 2)
/// The SWD paths are generated at run time, and ID_VENDOR_SWD_GEN is implemented.
#define CJTAG_CAP_SWD_CODEGEN (1U << 3)
/// ID_VENDOR_CJTAG_CONTROL takes mode 2: framing on, with the activation left to the host.
/// Cleared with CJTAG_CAP_OSCAN1; it is the one bit probe-rs checks before running cJTAG.
#define CJTAG_CAP_OSCAN1_ENTER (1U << 4)
/// Two-pin frames are generated at run time, and ID_VENDOR_JTAG_GEN is implemented.
#define CJTAG_CAP_JTAG_CODEGEN (1U << 5)
/// ID_VENDOR_DIO acts: a Pro whose J15 (USB-SIO off) was not fitted when the probe started. The
/// jumpers are read once at boot, so this describes the running image.
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
#ifdef SWD_CODEGEN
#define CJTAG_CAPS_SWD CJTAG_CAP_SWD_CODEGEN
#else
#define CJTAG_CAPS_SWD 0U
#endif
#ifdef JTAG_CODEGEN
#define CJTAG_CAPS_JTAG CJTAG_CAP_JTAG_CODEGEN
#else
#define CJTAG_CAPS_JTAG 0U
#endif
#define CJTAG_CAPS_BUILT (CJTAG_CAPS_SWD | CJTAG_CAPS_JTAG)

/** ID_VENDOR_SWD_GEN takes `latency, mode`. `latency` is the cycles from a rising-edge store to
 * the first load that sees the bit it launched, 1 to 64, or 0 to leave it as it is; setting it
 * regenerates and pins it, as mode 2 does. `mode` then:
 *
 *     0  nothing more
 *     1  measure the latency now, at the current clock, with DPIDR reads (src/swd_sweep.h)
 *     2  pin the latency: the probe stops measuring it on its own
 *     3  let the probe measure it on its own again
 *
 * It answers `status, latency, masked, ok`, then little-endian u32s: the clock asked for in Hz,
 * the write period and the read period in core cycles, the generated code's size in bytes, and
 * how many configurations were generated and how many could not be. Then the last measurement:
 *
 *     [29]      1 while the probe measures on its own, 0 once pinned
 *     [30]      what it found, LATENCY_* (latency.h)
 *     [31]      the latency it set, or 0 if it kept the one in use
 *     [32]      generations it ran reads with, at most 255
 *     [33]      read periods it measured at, the host's included
 *     [34..37]  the DPIDR value that read right
 *     [38..41]  core cycles it took
 *     [42..45]  measurements since the probe started
 *     [46..53]  u64: the latencies every read agrees with, bit n - 1 for latency n
 *     [54..57]  connections whose DPIDR read failed and read right once measured
 *
 * A measurement that ends with no latency agreeing with every read runs again, up to
 * LATENCY_ATTEMPTS in all; [32] and [33] count every attempt's.
 *
 * Status is DAP_ERROR for a latency or a mode out of range, which change nothing, and for a
 * measurement asked for while the port is not plain SWD, which does not run. `ok` is 0 when the
 * current configuration could not be generated and transfers run ARM's own code.
 */
#define SWD_GEN_MODE_NONE 0U
#define SWD_GEN_MODE_MEASURE 1U
#define SWD_GEN_MODE_PIN 2U
#define SWD_GEN_MODE_AUTOMATIC 3U

/** ID_VENDOR_JTAG_GEN takes `two_pin, four_wire, mode`: each wire's cycles from the falling edge
 * that launches TDO to the first load that sees it, 1 to 64, or 0 to leave it as it is. Setting
 * either regenerates the wire in use and pins both, as mode 2 does. `mode` is ID_VENDOR_SWD_GEN's,
 * for the wire in use: 1 measures it now with IDCODE reads of the TAP at the last transfer's chain
 * index (src/jtag_sweep.h), which leaves that TAP's IR at IDCODE.
 *
 * It answers `status, two_pin, four_wire, masked, ok`, then little-endian u32s: the clock asked for
 * in Hz, the period (a four-wire clock, or a two-pin packet) and a JTAG clock in core cycles, the
 * generated code's size in bytes, and how many configurations were generated and how many could
 * not be. Then the last measurement:
 *
 *     [30]      1 while the probe measures on its own, 0 once pinned
 *     [31]      what it found, LATENCY_* (latency.h)
 *     [32]      the wire it measured, JTAG_GEN_* (jtag_gen.h)
 *     [33]      the latency it measured, or 0 if it kept the one in use; two pins use one more, at
 *               most 64 (src/jtag_sweep.c)
 *     [34]      generations it ran reads with, at most 255, every attempt's (as ID_VENDOR_SWD_GEN)
 *     [35]      periods it measured at, the host's included, every attempt's
 *     [36..39]  the IDCODE that read right
 *     [40..43]  core cycles it took
 *     [44..47]  measurements since the probe started
 *     [48..55]  u64: the latencies every read agrees with, bit n - 1 for latency n
 *     [56..59]  connections whose first transfer failed and ran right once measured
 *     [60..63]  configurations copied back from a kept build with transfers, not generated
 *
 * Status is DAP_ERROR for a latency or a mode out of range, which change nothing, and for a
 * measurement asked for while the port is not JTAG, which does not run. `ok` is 0 when the current
 * configuration could not be generated and the wire runs ARM's own code; its bit 1 says whole
 * transfers were generated too, rather than run as scans, and bit 2 that their data bits are a
 * loop.
 */

/** ID_VENDOR_JTAG_PACING takes `mode`: 0 turns the learned idle clocks off, so the generated wire
 * is ARM's exactly, 1 turns them on (the default), anything else leaves them. It answers `status,
 * on, paced`: whether pacing is on and the idle clocks learned so far (jtag_gen_paced).
 */

/** ID_VENDOR_JTAG_LOG takes `n` and answers `status`, then generation n back from the newest, 0
 * the newest: the count of generations so far as a little-endian u32, then struct
 * jtag_gen_log_entry's fields in order, the last three little-endian (jtag_gen.h). Status is
 * DAP_ERROR, and nothing follows, for an n the log no longer holds.
 */

/** ID_VENDOR_DIO takes `mask, outputs, levels`, always all three, so that it can sit inside
 * DAP_ExecuteCommands. It sets the direction (1 an output) and the output level of each DIO pin in
 * `mask`, bit n for DIOn, and leaves the rest. A mask of 0 only reads. It answers:
 *
 *     [1]      status
 *     [2]      the pins the probe drives
 *     [3]      each pin's output latch: what it drives, or will once it is an output
 *     [4]      the levels on the probe's side of the translators, sampled as the command ends; an
 *              output reads our own drive
 *     [5]      the pins there are: 0 on a board without them or with J15 fitted
 *     [6]      the number of groups g
 *     [7..]    g masks, the pins that change direction together (Pro: 0x07, 0x08)
 *
 * Status is DAP_ERROR, and nothing changes, where there are no pins, for a pin that does not
 * exist, and for a result that would split a group. [2..] are the state as it stands either way.
 * The state outlives sessions and hosts, so a host that starts by reading with mask 0 sees what
 * one before it left driving. Each pin's header level moves at most once per command.
 */
#define DIO_REPLY_FIXED 7U

/** ID_VENDOR_STAGE takes `mode`: 0 has CPU 1 copy each command and reply itself, 1 has CPU 0 stage
 * them (the default), anything else leaves it. It answers `status, on`. Commands already received
 * keep the way they arrived, so the change reaches the ones after them.
 */

/** ID_VENDOR_TARGET takes `mode, channel` and answers `status, code, millivolts, detect`, the two
 * middle fields little-endian u16.
 *
 * Mode 0 is the target's supply as J6 pin 1 presents it, and channel is ignored. Mode 1 converts
 * any ADC0 channel, low five bits the channel and bit 7 for side B, and reports the millivolts at
 * the pin; it is how the supply's channel was found, and how to check it again. Detect is 1 while
 * something grounds J6 pin 9.
 *
 * Millivolts are against VREFP: the probe's own 3.3 V supply on the plain board, so a reading
 * there is only as absolute as that regulator, and a 3.0 V reference on the Pro. Status is
 * DAP_ERROR if the converter could not be brought up or did not answer, and the other fields are
 * then zero.
 */
#define TARGET_MODE_SUPPLY 0U
#define TARGET_MODE_CHANNEL 1U
#define TARGET_CHANNEL_SIDE_B 0x80U
#define TARGET_CHANNEL_MASK 0x1FU

/** The ADC0 channel DBGIF_VREF, PIO0_31, lands on. Found by sweeping every channel with the
 *  target powered and unpowered: see BENCH.md. */
#define TARGET_SUPPLY_CHANNEL 3U
#define TARGET_SUPPLY_SIDE_B false

/** ID_VENDOR_TARGET_POWER takes one mode byte: 0 off, 1 on, 2 read. It answers `status, enabled`.
 *  Status is DAP_ERROR unless target supply control is available and the mode is known. The state
 *  is the output latch's last value since startup; it does not confirm voltage at the target.
 */

/** R22/R23 halve pin 1 on its way to the ADC. */
#define TARGET_DIVIDER 2U

#if defined(SWD_CODEGEN) || defined(JTAG_CODEGEN)
/** `n` little-endian u32s at `out`; returns the byte after them. */
static uint8_t* put_u32s(uint8_t* out, const uint32_t* fields, uint32_t n) {
    for (uint32_t i = 0U; i < n; i++) {
        out[0] = (uint8_t)fields[i];
        out[1] = (uint8_t)(fields[i] >> 8);
        out[2] = (uint8_t)(fields[i] >> 16);
        out[3] = (uint8_t)(fields[i] >> 24);
        out += 4;
    }
    return out;
}
#endif

bool dap_vendor_on_aux;

uint32_t DAP_ProcessVendorCommand(const uint8_t* request, uint8_t* response) {
    const uint8_t id = request[0];

    // The echoed command id. Every handled case keeps it; the default case replaces it.
    response[0] = id;

    switch (id) {
    case ID_VENDOR_CJTAG_INFO: {
        uint8_t* out = response + 1;
        memcpy(out, CJTAG_INFO_MAGIC, sizeof(CJTAG_INFO_MAGIC) - 1U);
        out += sizeof(CJTAG_INFO_MAGIC) - 1U;
        *out++ = (uint8_t)CJTAG_INFO_VERSION;
        const uint32_t two_pin = (board_kind != BOARD_KIND_ON_BOARD) ? (CJTAG_CAP_OSCAN1 | CJTAG_CAP_OSCAN1_ENTER) : 0U;
        *out++ = (uint8_t)(two_pin | CJTAG_CAP_TARGET_SENSE | CJTAG_CAPS_BUILT |
                           (cJTAG_Active() ? CJTAG_CAP_OSCAN1_ACTIVE : 0U) | (dio_available() ? CJTAG_CAP_DIO : 0U) |
                           (target_power_available() ? CJTAG_CAP_TARGET_POWER : 0U));
        // The same string DAP_Info's product firmware version reports, so a host that knows
        // nothing about this command can still tell two builds apart.
        *out++ = (uint8_t)(sizeof(FIRMWARE_VERSION) - 1U);
        memcpy(out, FIRMWARE_VERSION, sizeof(FIRMWARE_VERSION) - 1U);
        out += sizeof(FIRMWARE_VERSION) - 1U;
        *out++ = board_kind;
        const uint32_t formats = (board_kind != BOARD_KIND_ON_BOARD) ? cJTAG_Offered() : 0U;
        for (uint32_t i = 0U; i < 4U; i++) {
            *out++ = (uint8_t)(formats >> (8U * i));
        }
        uint32_t const extended = (dap_shared.energy_available
                                       ? CJTAG_EXT_CAP_ENERGY | CJTAG_EXT_CAP_ENERGY_CALIBRATED |
                                             CJTAG_EXT_CAP_ENERGY_ACCUMULATED | CJTAG_EXT_CAP_MARKS
                                       : 0U) |
                                  (pro_bus_available() ? CJTAG_EXT_CAP_SPI | CJTAG_EXT_CAP_I2C : 0U) |
                                  (dio_available() ? CJTAG_EXT_CAP_DIO_AUX : 0U);
        for (uint32_t i = 0U; i < 4U; i++) {
            *out++ = (uint8_t)(extended >> (8U * i));
        }
        return (1U << 16) | (uint32_t)(out - response);
    }

    case ID_VENDOR_CJTAG_CONTROL: {
        // 0 returns to four-wire framing, and 2 frames OScan1 after an activation the host drove
        // itself. 1 is not a mode: the probe drives no activation of its own. 3 takes two more bytes, a scan format (SCNFMT) and the TAP state it starts in
        // (enum cjtag_tap), and frames that: the host has switched the target's format itself.
        // Mode 3 is a mode of its own rather than a byte after mode 2 because a vendor command
        // carries no length: an optional byte would be read from whatever command came next. 4
        // takes a u32 mask of the formats to offer from now until the probe restarts
        // (cJTAG_Offer), and frames nothing.
        const uint32_t mode = request[1];
        const uint32_t consumed = mode == 3U ? 4U : mode == 4U ? 6U : 2U;
        response[1] = DAP_OK;
        if ((mode != 0U) && (board_kind == BOARD_KIND_ON_BOARD)) {
            response[1] = DAP_ERROR;
        } else if (mode == 0U) {
            cJTAG_Deactivate();
        } else if (mode == 2U) {
            (void)cJTAG_Enter(CJTAG_OSCAN1, CJTAG_TAP_IDLE);
        } else if (mode == 4U) {
            cJTAG_Offer((uint32_t)request[2] | ((uint32_t)request[3] << 8) | ((uint32_t)request[4] << 16) |
                        ((uint32_t)request[5] << 24));
        } else if ((mode != 3U) || !cJTAG_Enter(request[2], request[3])) {
            response[1] = DAP_ERROR;
        }
#ifdef JTAG_CODEGEN
        // The wire changed: generate for it now rather than in the next command.
        if (DAP_Data.debug_port == DAP_PORT_JTAG) {
            (void)jtag_gen_ready(jtag_gen_wire());
        }
#endif
        return (consumed << 16) | 2U; // the id, the mode and its operands in; the id and a status out
    }

#ifdef MARKER_PINS
    case ID_VENDOR_J8: {
        // request[1] names the pins the host takes, request[2] the levels to drive them to. The
        // marker keeps whatever the host did not take, so handing it one pin leaves the other
        // three marking commands as usual. A mask of zero gives all four back.
        if (board_kind != BOARD_KIND_PLAIN) {
            response[1] = DAP_ERROR;
            response[2] = 0U;
            return (3U << 16) | 3U;
        }
        const uint32_t taken = (uint32_t)request[1] & J8_PIN_MASK;
        const uint32_t levels = (uint32_t)request[2] & taken;
        marker_mask = J8_PIN_MASK & ~taken;
        J8_GPIO->SET[J8_PORT] = levels;
        J8_GPIO->CLR[J8_PORT] = taken & ~levels;
        response[1] = DAP_OK;
        // What the pins actually read, so a host sees the levels rather than trusting the write.
        response[2] = (uint8_t)(J8_GPIO->PIN[J8_PORT] & J8_PIN_MASK);
        return (3U << 16) | 3U;
    }
#endif

    case ID_VENDOR_DIO: {
        response[1] = dio_set(request[1], request[2], request[3]) ? DAP_OK : DAP_ERROR;
        dio_state_t state;
        dio_get(&state);
        response[2] = state.outputs;
        response[3] = state.levels;
        response[4] = state.read;
        response[5] = state.pins;
        response[6] = state.groups;
        memcpy(response + DIO_REPLY_FIXED, state.group, state.groups);
        return (4U << 16) | (DIO_REPLY_FIXED + state.groups);
    }

    case ID_VENDOR_REBOOT:
        // Only the two defined modes act. Anything else is refused rather than guessed at,
        // because the failure costs the probe until someone replugs it.
        if (request[1] == REBOOT_FIRMWARE || request[1] == REBOOT_BOOTLOADER) {
            reboot_request(request[1]);
            response[1] = DAP_OK;
        } else {
            response[1] = DAP_ERROR;
        }
        return (2U << 16) | 2U;

    case ID_VENDOR_STAGE:
        if (request[1] <= 1U) {
            dap_shared.stage_commands = request[1];
        }
        response[1] = DAP_OK;
        response[2] = dap_shared.stage_commands;
        return (2U << 16) | 3U;

#ifdef DAP_TIMING_DIAGNOSTICS
    case ID_VENDOR_TIMING: {
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
        return (2U << 16) | (3U + sizeof(values));
    }
#endif

    case ID_VENDOR_TARGET: {
        const bool supply = (request[1] == TARGET_MODE_SUPPLY);
        const uint8_t channel = supply ? TARGET_SUPPLY_CHANNEL : (uint8_t)(request[2] & TARGET_CHANNEL_MASK);
        const bool side_b = supply ? TARGET_SUPPLY_SIDE_B : ((request[2] & TARGET_CHANNEL_SIDE_B) != 0U);
        uint16_t code = 0U;
        uint32_t mv = 0U;
        target_sense_status_t status = TARGET_SENSE_OK;
        if (request[1] <= TARGET_MODE_CHANNEL) {
            status = target_sense_convert(channel, side_b, &code);
        }
        if (request[1] > TARGET_MODE_CHANNEL || status != TARGET_SENSE_OK) {
            response[1] = DAP_ERROR;
            code = (uint16_t)status;
        } else {
            response[1] = DAP_OK;
            mv = ((uint32_t)code * target_sense_full_scale_mv()) >> 16;
            if (supply) {
                mv *= TARGET_DIVIDER;
            }
        }
        response[2] = (uint8_t)code;
        response[3] = (uint8_t)(code >> 8);
        response[4] = (uint8_t)mv;
        response[5] = (uint8_t)(mv >> 8);
        response[6] = target_sense_detected() ? 1U : 0U;
        return (3U << 16) | 7U;
    }

    case ID_VENDOR_TARGET_POWER: {
        const uint8_t mode = request[1];
        bool ok = target_power_available();
        if (ok && mode == TARGET_POWER_MODE_OFF) {
            ok = target_power_set(false);
        } else if (ok && mode == TARGET_POWER_MODE_ON) {
            ok = target_power_set(true);
        } else if (mode != TARGET_POWER_MODE_READ) {
            ok = false;
        }
        response[1] = ok ? DAP_OK : DAP_ERROR;
        response[2] = target_power_enabled() ? 1U : 0U;
        return (2U << 16) | 3U;
    }

    case ID_VENDOR_ENERGY: {
        uint8_t const mode = request[1];
        response[1] = DAP_OK;
        if ((mode == 0U) || (mode == 5U)) {
            uint32_t const rate = (uint32_t)request[2] | ((uint32_t)request[3] << 8U) |
                                  ((uint32_t)request[4] << 16U) | ((uint32_t)request[5] << 24U);
            // Mode 5 starts a stream with the capture, and only Verge Aux carries a stream. Its last
            // byte is the DIO pins whose edges the stream reports.
            if ((mode == 5U) && !dap_vendor_on_aux) {
                response[1] = DAP_ERROR;
                return (7U << 16) | 2U;
            }
            energy_monitor_status_t const status = (mode == 5U) ? energy_monitor_start_stream(rate, request[6])
                                                                : energy_monitor_start(rate);
            uint32_t const actual_rate = energy_monitor_rate();
            response[2] = (uint8_t)status;
            response[3] = (uint8_t)actual_rate;
            response[4] = (uint8_t)(actual_rate >> 8U);
            response[5] = (uint8_t)(actual_rate >> 16U);
            response[6] = (uint8_t)(actual_rate >> 24U);
            return ((mode == 5U) ? (7U << 16) : (6U << 16)) | 7U;
        }
        if (mode == 1U) {
            energy_monitor_sample_t sample = {0};
            energy_monitor_status_t const status = energy_monitor_latest(&sample);
            response[2] = (uint8_t)status;
            response[3] = (uint8_t)sample.voltage_code;
            response[4] = (uint8_t)(sample.voltage_code >> 8U);
            response[5] = (uint8_t)sample.current_code;
            response[6] = (uint8_t)(sample.current_code >> 8U);
            response[7] = (uint8_t)sample.sequence;
            response[8] = (uint8_t)(sample.sequence >> 8U);
            response[9] = (uint8_t)(sample.sequence >> 16U);
            response[10] = (uint8_t)(sample.sequence >> 24U);
            response[11] = (uint8_t)sample.voltage_mv;
            response[12] = (uint8_t)(sample.voltage_mv >> 8U);
            response[13] = (uint8_t)(sample.voltage_mv >> 16U);
            response[14] = (uint8_t)(sample.voltage_mv >> 24U);
            response[15] = (uint8_t)sample.current_na;
            response[16] = (uint8_t)((uint32_t)sample.current_na >> 8U);
            response[17] = (uint8_t)((uint32_t)sample.current_na >> 16U);
            response[18] = (uint8_t)((uint32_t)sample.current_na >> 24U);
            response[19] = sample.range_id;
            response[20] = sample.calibrated ? 1U : 0U;
            return (2U << 16) | 21U;
        }
        if (mode == 2U) {
            energy_monitor_status_t const status = dap_shared.energy_available
                                                       ? energy_monitor_stop() : ENERGY_MONITOR_UNAVAILABLE;
            response[2] = (uint8_t)status;
            return (2U << 16) | 3U;
        }
        if (mode == 3U) {
            uint64_t energy_nj = 0U;
            uint64_t pairs = 0U;
            energy_monitor_status_t const status =
                energy_monitor_accumulated(&energy_nj, &pairs);
            response[2] = (uint8_t)status;
            for (uint32_t i = 0U; i < 8U; i++) {
                response[3U + i] = (uint8_t)(energy_nj >> (8U * i));
                response[11U + i] = (uint8_t)(pairs >> (8U * i));
            }
            return (2U << 16) | 19U;
        }
        if (mode == 4U) {
            energy_calibration_t table = {0};
            energy_monitor_status_t const status =
                request[2] > 1U ? ENERGY_MONITOR_BAD_ARGUMENT : energy_monitor_calibration(request[2] == 1U, &table);
            response[2] = (uint8_t)status;
            response[3] = table.count;
            for (uint32_t i = 0U; i < 4U; i++) {
                uint8_t* const point = response + 4U + (6U * i);
                point[0] = (uint8_t)table.code[i];
                point[1] = (uint8_t)(table.code[i] >> 8U);
                for (uint32_t j = 0U; j < 4U; j++) {
                    point[2U + j] = (uint8_t)(table.current_na[i] >> (8U * j));
                }
            }
            return (3U << 16) | 28U;
        }
        response[1] = DAP_ERROR;
        return (2U << 16) | 2U;
    }

    case ID_VENDOR_MARK: {
        // dap_packet.h's plan has checked that the label is all there.
        uint8_t const length = request[5];
        uint32_t const consumed = 6U + length;
        bool available = false;
        uint8_t result = 0U;
        if (length <= ENERGY_STREAM_LABEL_MAX) {
            uint32_t const number = (uint32_t)request[1] | ((uint32_t)request[2] << 8U) |
                                    ((uint32_t)request[3] << 16U) | ((uint32_t)request[4] << 24U);
            result = energy_monitor_mark(number, request + 6, length, &available);
        }
        if (!available) {
            response[1] = DAP_ERROR;
            return (consumed << 16) | 2U;
        }
        response[1] = DAP_OK;
        response[2] = result;
        return (consumed << 16) | 3U;
    }

    case ID_VENDOR_SPI:
        return pro_spi_command(request, response);

    case ID_VENDOR_I2C:
        return pro_i2c_command(request, response);

    case ID_VENDOR_SAMPLE:
        return sample_probe_command(request, response);


#ifdef CODE_LAB
    case ID_VENDOR_LAB:
        return code_lab_command(request, response);
#endif

#ifdef SWD_CODEGEN
    case ID_VENDOR_SWD_GEN: {
        const uint32_t latency = request[1];
        const uint32_t mode = request[2];
        response[1] = DAP_OK;
        if ((latency > LATENCY_MAX) || (mode > SWD_GEN_MODE_AUTOMATIC)) {
            response[1] = DAP_ERROR;
        } else {
            if (latency != 0U) {
                swd_gen_latency = latency;
                swd_sweep_automatic = false;
                swd_gen_changed();
            }
            if (mode == SWD_GEN_MODE_PIN) {
                swd_sweep_automatic = false;
            } else if (mode == SWD_GEN_MODE_AUTOMATIC) {
                swd_sweep_automatic = true;
            } else if (mode == SWD_GEN_MODE_MEASURE) {
                if ((DAP_Data.debug_port == DAP_PORT_SWD) && !cJTAG_Active()) {
                    swd_sweep();
                } else {
                    response[1] = DAP_ERROR;
                }
            }
        }
        (void)swd_gen_ready();
        response[2] = (uint8_t)swd_gen_latency;
        response[3] = swd_gen_info.masked ? 1U : 0U;
        response[4] = swd_gen_info.ok ? 1U : 0U;
        const struct latency_result* measured = &swd_sweep_last.result;
        const uint32_t fields[] = {swd_gen_clock_hz,         swd_gen_info.write_period, swd_gen_info.read_period,
                                   swd_gen_info.code_bytes, swd_gen_builds,            swd_gen_failures};
        uint8_t* out = put_u32s(response + 5, fields, sizeof fields / sizeof fields[0]);
        *out++ = swd_sweep_automatic ? 1U : 0U;
        *out++ = (uint8_t)measured->outcome;
        *out++ = (uint8_t)measured->latency;
        *out++ = (uint8_t)(measured->tries < 255U ? measured->tries : 255U);
        *out++ = (uint8_t)measured->periods;
        const uint32_t sweep[] = {measured->word,
                                  swd_sweep_last.cycles,
                                  swd_sweep_last.runs,
                                  (uint32_t)measured->consistent,
                                  (uint32_t)(measured->consistent >> 32),
                                  swd_sweep_last.rescues};
        out = put_u32s(out, sweep, sizeof sweep / sizeof sweep[0]);
        return (3U << 16) | (uint32_t)(out - response);
    }
#endif

#ifdef JTAG_CODEGEN
    case ID_VENDOR_JTAG_GEN: {
        const uint32_t two_pin = request[1];
        const uint32_t four_wire = request[2];
        const uint32_t mode = request[3];
        response[1] = DAP_OK;
        if ((two_pin > LATENCY_MAX) || (four_wire > LATENCY_MAX) || (mode > SWD_GEN_MODE_AUTOMATIC)) {
            response[1] = DAP_ERROR;
        } else {
            if (two_pin != 0U) {
                jtag_gen_latency[JTAG_GEN_TWO_PIN] = two_pin;
                jtag_sweep_automatic = false;
            }
            if (four_wire != 0U) {
                jtag_gen_latency[JTAG_GEN_FOUR_WIRE] = four_wire;
                jtag_sweep_automatic = false;
            }
            if (mode == SWD_GEN_MODE_PIN) {
                jtag_sweep_automatic = false;
            } else if (mode == SWD_GEN_MODE_AUTOMATIC) {
                jtag_sweep_automatic = true;
            } else if (mode == SWD_GEN_MODE_MEASURE) {
                if (DAP_Data.debug_port == DAP_PORT_JTAG) {
                    jtag_sweep();
                } else {
                    response[1] = DAP_ERROR;
                }
            }
        }
        (void)jtag_gen_ready(jtag_gen_wire());
        response[2] = (uint8_t)jtag_gen_latency[JTAG_GEN_TWO_PIN];
        response[3] = (uint8_t)jtag_gen_latency[JTAG_GEN_FOUR_WIRE];
        response[4] = jtag_gen_info.masked ? 1U : 0U;
        response[5] = (jtag_gen_info.ok ? 1U : 0U) | (jtag_gen_info.transfers ? 2U : 0U) |
                      (jtag_gen_info.looped ? 4U : 0U);
        const uint32_t fields[] = {jtag_gen_clock_hz,       jtag_gen_info.period, jtag_gen_info.frame,
                                   jtag_gen_info.code_bytes, jtag_gen_builds,      jtag_gen_failures};
        uint8_t* out = put_u32s(response + 6, fields, sizeof fields / sizeof fields[0]);
        const struct latency_result* measured = &jtag_sweep_last.result;
        *out++ = jtag_sweep_automatic ? 1U : 0U;
        *out++ = (uint8_t)measured->outcome;
        *out++ = (uint8_t)jtag_sweep_last.wire;
        *out++ = (uint8_t)measured->latency;
        *out++ = (uint8_t)(measured->tries < 255U ? measured->tries : 255U);
        *out++ = (uint8_t)measured->periods;
        const uint32_t sweep[] = {measured->word,
                                  jtag_sweep_last.cycles,
                                  jtag_sweep_last.runs,
                                  (uint32_t)measured->consistent,
                                  (uint32_t)(measured->consistent >> 32),
                                  jtag_sweep_last.rescues,
                                  jtag_gen_restores};
        out = put_u32s(out, sweep, sizeof sweep / sizeof sweep[0]);
        return (4U << 16) | (uint32_t)(out - response);
    }

    case ID_VENDOR_JTAG_LOG: {
        const uint32_t count = jtag_gen_builds + jtag_gen_failures + jtag_gen_restores;
        const uint32_t back = request[1];
        if ((back >= count) || (back >= JTAG_GEN_LOG_SIZE)) {
            response[1] = DAP_ERROR;
            return (2U << 16) | 2U;
        }
        const struct jtag_gen_log_entry* entry = &jtag_gen_log[(count - 1U - back) % JTAG_GEN_LOG_SIZE];
        response[1] = DAP_OK;
        uint8_t* out = put_u32s(response + 2, &count, 1U);
        const uint8_t bytes[] = {entry->command, entry->wire,   entry->changed, entry->flags, entry->latency,
                                 entry->before,  entry->after,  entry->idle,    entry->paced};
        memcpy(out, bytes, sizeof bytes);
        out += sizeof bytes;
        *out++ = (uint8_t)entry->period;
        *out++ = (uint8_t)(entry->period >> 8);
        const uint32_t times[] = {entry->at, entry->cycles};
        out = put_u32s(out, times, 2U);
        return (2U << 16) | (uint32_t)(out - response);
    }

    case ID_VENDOR_JTAG_PACING:
        if (request[1] <= 1U) {
            jtag_gen_pacing = request[1] != 0U;
            if (DAP_Data.debug_port == DAP_PORT_JTAG) {
                (void)jtag_gen_ready(jtag_gen_wire());
            }
        }
        response[1] = DAP_OK;
        response[2] = jtag_gen_pacing ? 1U : 0U;
        response[3] = (uint8_t)jtag_gen_paced;
        return (2U << 16) | 4U;
#endif

    default:
        // DAP_ProcessCommand hands this return value straight back to the host, so an id we
        // do not answer has to say so itself. A host that got its own id back would read
        // that as success.
        response[0] = ID_DAP_Invalid;
        return (1U << 16) | 1U;
    }
}
