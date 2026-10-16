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

/// Answered by ID_VENDOR_CJTAG_INFO, so a host can recognise this firmware rather than assume it.
#define CJTAG_INFO_MAGIC "CJTAG"
#define CJTAG_INFO_VERSION 1U

uint32_t DAP_ProcessVendorCommand(const uint8_t* request, uint8_t* response) {
    const uint8_t id = request[0];

    // The echoed command id. Every handled case keeps it; the default case replaces it.
    response[0] = id;

    switch (id) {
    case ID_VENDOR_CJTAG_INFO: {
        // magic, format version, and whether OScan1 is active right now
        uint8_t* out = response + 1;
        memcpy(out, CJTAG_INFO_MAGIC, sizeof(CJTAG_INFO_MAGIC) - 1U);
        out += sizeof(CJTAG_INFO_MAGIC) - 1U;
        *out++ = (uint8_t)CJTAG_INFO_VERSION;
        *out++ = cJTAG_Active() ? 1U : 0U;
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
