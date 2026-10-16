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
#include "cjtag.h"
#include "marker.h"
#include "reboot.h"
#include "sample_probe.h"
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

/** Answered by ID_VENDOR_CJTAG_INFO, so a host can recognise this firmware rather than assume it.
 *
 * The reply, after the echoed command id:
 *
 *     [0..4]  "CJTAG"                 magic
 *     [5]     format version          this layout is 2
 *     [6]     capability flags        CJTAG_CAP_*
 *     [7]     build string length n
 *     [8..]   build string, n bytes, not terminated
 *
 * A host checks the magic to know it is us, the flags to know what it may use, and the build
 * string to know which image it is talking to. The capability flags are what makes this worth
 * having over a version number alone: a feature can arrive without every host learning a new
 * version, and a host can refuse to use something an older image does not have.
 *
 * Version 1 had no flags and no build string, and existed only in images that were never flashed.
 */
#define CJTAG_INFO_MAGIC "CJTAG"
#define CJTAG_INFO_VERSION 2U

/// Two-pin OScan1 JTAG is implemented. The reason this firmware exists, so always set for now.
#define CJTAG_CAP_OSCAN1 (1U << 0)
/// OScan1 framing is on right now, which is ID_VENDOR_CJTAG_CONTROL's state.
#define CJTAG_CAP_OSCAN1_ACTIVE (1U << 1)
/// ID_VENDOR_TARGET is implemented.
#define CJTAG_CAP_TARGET_SENSE (1U << 2)

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
        *out++ = (uint8_t)(CJTAG_CAP_OSCAN1 | CJTAG_CAP_TARGET_SENSE | (cJTAG_Active() ? CJTAG_CAP_OSCAN1_ACTIVE : 0U));
        // The same string DAP_Info's product firmware version reports, so a host that knows
        // nothing about this command can still tell two builds apart.
        *out++ = (uint8_t)(sizeof(FIRMWARE_VERSION) - 1U);
        memcpy(out, FIRMWARE_VERSION, sizeof(FIRMWARE_VERSION) - 1U);
        out += sizeof(FIRMWARE_VERSION) - 1U;
        return (1U << 16) | (uint32_t)(out - response);
    }

    case ID_VENDOR_CJTAG_CONTROL:
        if (request[1] != 0U) {
            cJTAG_Activate();
        } else {
            cJTAG_Deactivate();
        }
        response[1] = DAP_OK;
        return (2U << 16) | 2U; // the id and the mode byte in, the id and a status out

#ifdef MARKER_PINS
    case ID_VENDOR_J8: {
        // request[1] names the pins the host takes, request[2] the levels to drive them to. The
        // marker keeps whatever the host did not take, so handing it one pin leaves the other
        // three marking commands as usual. A mask of zero gives all four back.
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

    default:
        // DAP_ProcessCommand hands this return value straight back to the host, so an id we
        // do not answer has to say so itself. A host that got its own id back would read
        // that as success.
        response[0] = ID_DAP_Invalid;
        return (1U << 16) | 1U;
    }
}
