#ifndef DAP_HID_H
#define DAP_HID_H

#include <stdint.h>
#include <string.h>

#include "dap_shared.h"

static inline uint32_t dap_hid_reply(const uint8_t* request, uint32_t request_length,
                                     uint8_t* response, uint32_t produced) {
    if (produced > DAP_SHARED_HID_REPORT_SIZE) {
        produced = DAP_SHARED_HID_REPORT_SIZE;
    }
    // DAP_Info's packet size describes this transport, even when bulk is also enabled.
    if (request_length >= 2U && request[0] == 0x00U && request[1] == 0xFFU &&
        produced >= 4U && response[0] == 0x00U && response[1] == 2U) {
        response[2] = (uint8_t)DAP_SHARED_HID_REPORT_SIZE;
        response[3] = (uint8_t)(DAP_SHARED_HID_REPORT_SIZE >> 8U);
    }
    memset(response + produced, 0, DAP_SHARED_HID_REPORT_SIZE - produced);
    return DAP_SHARED_HID_REPORT_SIZE;
}

#endif
