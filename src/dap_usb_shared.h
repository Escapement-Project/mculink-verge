#ifndef DAP_USB_SHARED_H
#define DAP_USB_SHARED_H

#include <stddef.h>
#include <stdint.h>

#include "dap_usb_slots.h"

#ifndef REQUEST_SLOTS
#define REQUEST_SLOTS 3U
#endif

typedef struct {
    volatile uint16_t request_len[REQUEST_SLOTS];
    volatile uint32_t request_seq[REQUEST_SLOTS];
    volatile uint8_t request_head;
    volatile uint8_t request_tail;
    volatile uint8_t rx_armed_slot;
    volatile uint32_t next_request_seq;
    volatile uint16_t hid_request_len;
    volatile uint32_t hid_request_seq;
    volatile uint16_t response_len;
    volatile uint8_t response_transport;
    volatile dap_usb_response_state_t response_state;
} dap_usb_shared_state_t;

/** Clear shared state placed in USB SRAM, which startup leaves uninitialized. */
static inline void dap_usb_shared_init(volatile dap_usb_shared_state_t* state) {
    volatile uint8_t* bytes = (volatile uint8_t*)state;
    for (size_t i = 0U; i < sizeof(*state); i++) {
        bytes[i] = 0U;
    }
}

#endif
