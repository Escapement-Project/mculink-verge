/** Vendor commands.
 *
 * CMSIS-DAP leaves 0x80..0x9F to the probe, so anything from 0x82 up is free.
 *
 * Stock answers vendor 0x80 and 0x81. Neither is
 * implemented here, so any LinkServer feature resting on them will not work against this image.
 */
#include <string.h>

// DAP_config.h first: DAP.h reads the DAP_SWD and DAP_JTAG settings it defines, so a sorted
// include block that puts DAP.h ahead of it will not compile.
#include "DAP_config.h"

#include "DAP.h"
#include "board_pins.h"
#include "cjtag.h"
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

/** Answered by ID_VENDOR_CJTAG_INFO, so a host can recognise this firmware rather than assume it.
 *
 * The reply, after the echoed command id:
 *
 *     [0..4]  "CJTAG"                 magic
 *     [5]     format version          this layout is 3
 *     [6]     capability flags        CJTAG_CAP_*
 *     [7]     build string length n
 *     [8..]   build string, n bytes, not terminated
 *     [8+n]   board                   BOARD_KIND_* (board_pins.h); format 3 on
 *
 * A host checks the magic to know it is us, the flags to know what it may use, and the build
 * string to know which image it is talking to. The capability flags are what makes this worth
 * having over a version number alone: a feature can arrive without every host learning a new
 * version, and a host can refuse to use something an older image does not have.
 *
 * Fields are only ever appended. probe-rs reads every format as format 2 and ignores what follows
 * the build string, so a field that moved would be misread without a word. A host reads a field
 * only at the format that added it: a v1 HID reply is padded, so a format 2 image has a byte after
 * the build string too.
 *
 * Version 1 had no flags and no build string, and existed only in images that were never flashed.
 */
#define CJTAG_INFO_MAGIC "CJTAG"
#define CJTAG_INFO_VERSION 3U

/// Two-pin OScan1 JTAG is implemented. Cleared on an on-board MCU-Link, where nobody has framed
/// OScan1 through the buffers yet.
#define CJTAG_CAP_OSCAN1 (1U << 0)
/// OScan1 framing is on right now, which is ID_VENDOR_CJTAG_CONTROL's state.
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
 *     [32]      the wire it measured, 0 four-wire, 1 two-pin
 *     [33]      the latency it set, or 0 if it kept the one in use
 *     [34]      generations it ran reads with, at most 255
 *     [35]      periods it measured at, the host's included
 *     [36..39]  the IDCODE that read right
 *     [40..43]  core cycles it took
 *     [44..47]  measurements since the probe started
 *     [48..55]  u64: the latencies every read agrees with, bit n - 1 for latency n
 *     [56..59]  connections whose first transfer failed and ran right once measured
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

/** ID_VENDOR_TARGET takes `mode, channel` and answers `status, code, millivolts, detect`, the two
 * middle fields little-endian u16.
 *
 * Mode 0 is the target's supply as J6 pin 1 presents it, and channel is ignored. Mode 1 converts
 * any ADC0 channel, low five bits the channel and bit 7 for side B, and reports the millivolts at
 * the pin; it is how the supply's channel was found, and how to check it again. Detect is 1 while
 * something grounds J6 pin 9.
 *
 * Millivolts are against the probe's own supply taken as 3300, which is what VDDA and VREFP are
 * tied to, so a reading is only as absolute as that regulator. Status is DAP_ERROR if the
 * converter could not be brought up or did not answer, and the other fields are then zero.
 */
#define TARGET_MODE_SUPPLY 0U
#define TARGET_MODE_CHANNEL 1U
#define TARGET_CHANNEL_SIDE_B 0x80U
#define TARGET_CHANNEL_MASK 0x1FU

/** The ADC0 channel DBGIF_VREF, PIO0_31, lands on. Found by sweeping every channel with the
 *  target powered and unpowered: see BENCH.md. */
#define TARGET_SUPPLY_CHANNEL 3U
#define TARGET_SUPPLY_SIDE_B false

/** Full scale, the probe's own 3V3, and R22/R23 halving pin 1 on its way to the ADC. */
#define TARGET_FULL_SCALE_MV 3300U
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
        const uint32_t two_pin = (board_kind == BOARD_KIND_PLAIN) ? (CJTAG_CAP_OSCAN1 | CJTAG_CAP_OSCAN1_ENTER) : 0U;
        *out++ = (uint8_t)(two_pin | CJTAG_CAP_TARGET_SENSE | CJTAG_CAPS_BUILT |
                           (cJTAG_Active() ? CJTAG_CAP_OSCAN1_ACTIVE : 0U));
        // The same string DAP_Info's product firmware version reports, so a host that knows
        // nothing about this command can still tell two builds apart.
        *out++ = (uint8_t)(sizeof(FIRMWARE_VERSION) - 1U);
        memcpy(out, FIRMWARE_VERSION, sizeof(FIRMWARE_VERSION) - 1U);
        out += sizeof(FIRMWARE_VERSION) - 1U;
        *out++ = board_kind;
        return (1U << 16) | (uint32_t)(out - response);
    }

    case ID_VENDOR_CJTAG_CONTROL:
        // 0 returns to four-wire framing, 1 drives OpenOCD's activation and frames, 2 frames
        // after an activation the host drove itself. A target whose adapter powers up online, as
        // the CC13xx does, wants 2: OpenOCD's activation is for one that powers up offline.
        response[1] = DAP_OK;
        switch (request[1]) {
        case 0U:
            cJTAG_Deactivate();
            break;
        case 1U:
        case 2U:
            if (board_kind != BOARD_KIND_PLAIN) {
                response[1] = DAP_ERROR;
            } else if (request[1] == 1U) {
                cJTAG_Activate();
            } else {
                cJTAG_Enter();
            }
            break;
        default:
            response[1] = DAP_ERROR;
            break;
        }
#ifdef JTAG_CODEGEN
        // The wire changed: generate for it now rather than in the next command.
        if (DAP_Data.debug_port == DAP_PORT_JTAG) {
            (void)jtag_gen_ready(cJTAG_Active() ? JTAG_GEN_TWO_PIN : JTAG_GEN_FOUR_WIRE);
        }
#endif
        return (2U << 16) | 2U; // the id and the mode byte in, the id and a status out

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

    case ID_VENDOR_TARGET: {
        const bool supply = (request[1] == TARGET_MODE_SUPPLY);
        const uint8_t channel = supply ? TARGET_SUPPLY_CHANNEL : (uint8_t)(request[2] & TARGET_CHANNEL_MASK);
        const bool side_b = supply ? TARGET_SUPPLY_SIDE_B : ((request[2] & TARGET_CHANNEL_SIDE_B) != 0U);
        uint16_t code = 0U;
        uint32_t mv = 0U;
        if (request[1] > TARGET_MODE_CHANNEL || !target_sense_convert(channel, side_b, &code)) {
            response[1] = DAP_ERROR;
            code = 0U;
        } else {
            response[1] = DAP_OK;
            mv = ((uint32_t)code * TARGET_FULL_SCALE_MV) >> 16;
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
        (void)jtag_gen_ready(cJTAG_Active() ? JTAG_GEN_TWO_PIN : JTAG_GEN_FOUR_WIRE);
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
                                  jtag_sweep_last.rescues};
        out = put_u32s(out, sweep, sizeof sweep / sizeof sweep[0]);
        return (4U << 16) | (uint32_t)(out - response);
    }

    case ID_VENDOR_JTAG_PACING:
        if (request[1] <= 1U) {
            jtag_gen_pacing = request[1] != 0U;
            if (DAP_Data.debug_port == DAP_PORT_JTAG) {
                (void)jtag_gen_ready(cJTAG_Active() ? JTAG_GEN_TWO_PIN : JTAG_GEN_FOUR_WIRE);
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
