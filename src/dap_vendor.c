/** Vendor commands.
 *
 * CMSIS-DAP leaves 0x80..0x9F to the probe, so anything from 0x82 up is free.
 *
 * Stock answers vendor 0x80 and 0x81.
 * Neither is implemented here, so any LinkServer feature resting on
 * them will not work against this image.
 */
#include <string.h>

// DAP_config.h first: DAP.h reads the DAP_SWD and DAP_JTAG settings it defines, so a sorted
// include block that puts DAP.h ahead of it will not compile.
#include "DAP_config.h"

#include "DAP.h"
#include "cjtag.h"

/// Identifies this firmware and says what it can do. Read-only, so a host may send it blindly.
#define ID_VENDOR_CJTAG_INFO ID_DAP_Vendor2
/// Turns OScan1 framing on and off.
#define ID_VENDOR_CJTAG_CONTROL ID_DAP_Vendor3

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
        *out++ = (uint8_t)(CJTAG_CAP_OSCAN1 | (cJTAG_Active() ? CJTAG_CAP_OSCAN1_ACTIVE : 0U));
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

    default:
        // DAP_ProcessCommand hands this return value straight back to the host, so an id we
        // do not answer has to say so itself. A host that got its own id back would read
        // that as success.
        response[0] = ID_DAP_Invalid;
        return (1U << 16) | 1U;
    }
}
