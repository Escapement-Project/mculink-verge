/** Vendor commands.
 *
 * CMSIS-DAP leaves 0x80..0x9F to the probe, so anything from 0x82 up is free.
 *
 * What stock's 0x80 and 0x81 do is not known, and this firmware does not implement them. Any
 * LinkServer feature that depends on them will not work here.
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
    uint32_t num = (1U << 16) | 1U; // one request byte consumed, one response byte produced

    *response++ = *request;

    switch (*request++) {
    case ID_VENDOR_CJTAG_INFO: {
        // magic, format version, and whether OScan1 is active right now
        memcpy(response, CJTAG_INFO_MAGIC, sizeof(CJTAG_INFO_MAGIC) - 1U);
        response += sizeof(CJTAG_INFO_MAGIC) - 1U;
        *response++ = (uint8_t)CJTAG_INFO_VERSION;
        *response++ = cJTAG_Active() ? 1U : 0U;
        num += sizeof(CJTAG_INFO_MAGIC) - 1U + 2U;
        break;
    }

    case ID_VENDOR_CJTAG_CONTROL: {
        uint8_t enable = *request;
        num += 1U << 16; // the mode byte

        if (enable != 0U) {
            cJTAG_Activate();
        } else {
            cJTAG_Deactivate();
        }
        *response = DAP_OK;
        num += 1U;
        break;
    }

    default:
        return (1U << 16) | 1U; // the caller turns an unhandled id into ID_DAP_Invalid
    }

    return num;
}
