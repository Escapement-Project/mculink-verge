#ifndef DAP_HID_H
#define DAP_HID_H

#include <stdint.h>
#include <string.h>

#include "dap_shared.h"

static inline uint32_t dap_hid_reply(uint8_t* response, uint32_t produced) {
    if (produced > DAP_SHARED_HID_REPORT_SIZE) {
        produced = DAP_SHARED_HID_REPORT_SIZE;
    }
    memset(response + produced, 0, DAP_SHARED_HID_REPORT_SIZE - produced);
    return DAP_SHARED_HID_REPORT_SIZE;
}

#endif
