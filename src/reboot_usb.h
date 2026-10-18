#ifndef REBOOT_USB_H
#define REBOOT_USB_H

#include <stdbool.h>
#include <stdint.h>

#define REBOOT_USB_VENDOR_REQUEST 0x03U
#define REBOOT_USB_REQUEST_TYPE 0x40U

static inline bool reboot_usb_control_request_valid(uint8_t request_type, uint8_t request, uint16_t mode,
                                                    uint16_t index, uint16_t length) {
    return request_type == REBOOT_USB_REQUEST_TYPE && request == REBOOT_USB_VENDOR_REQUEST && mode <= 1U &&
           index == 0U && length == 0U;
}

static inline bool reboot_wait_for_response(bool from_usb_control, bool response_idle) {
    return !from_usb_control && !response_idle;
}

#endif
