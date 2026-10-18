#ifndef DAP_USB_SLOTS_H
#define DAP_USB_SLOTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DAP_USB_BUFFER_ALIGN 64U
#define DAP_USB_RESPONSE_SLOT_SIZE(size) \
    (((size) + DAP_USB_BUFFER_ALIGN - 1U) & ~(DAP_USB_BUFFER_ALIGN - 1U))

typedef bool (*dap_usb_rx_start_fn)(void* context, uint8_t slot);
typedef bool (*dap_usb_tx_start_fn)(void* context, uint8_t slot, uint16_t length);

typedef enum {
    DAP_USB_RESPONSE_FREE,
    DAP_USB_RESPONSE_READY,
    DAP_USB_RESPONSE_IN_FLIGHT,
} dap_usb_response_state_t;

static inline uint8_t dap_usb_next_slot(uint8_t slot, uint8_t count) {
    return (uint8_t)((slot + 1U) % count);
}

static inline bool dap_usb_sequence_before(uint32_t left, uint32_t right) {
    return (int32_t)(left - right) < 0;
}

static inline bool dap_usb_request_slot_free(volatile uint16_t const* request_len, uint8_t slot) {
    return request_len[slot] == 0U;
}

static inline bool dap_usb_choose_hid(bool hid_ready, uint32_t hid_seq, bool bulk_ready, uint32_t bulk_seq) {
    return hid_ready && (!bulk_ready || dap_usb_sequence_before(hid_seq, bulk_seq));
}

static inline uint8_t dap_usb_request_release(volatile uint16_t* request_len, uint8_t head, uint8_t count) {
    request_len[head] = 0U;
    return dap_usb_next_slot(head, count);
}

/** Publish the slot before arming USB, because completion may interrupt the caller. */
static inline bool dap_usb_arm_rx(bool available, uint8_t tail, volatile uint8_t* armed_slot,
                                  dap_usb_rx_start_fn start, void* context) {
    if (!available) {
        return false;
    }

    *armed_slot = tail;
    return start(context, tail);
}

/** Reserve the reply before starting USB, because its completion can interrupt the caller. */
static inline bool dap_usb_start_tx(volatile bool* busy, uint16_t length,
                                    dap_usb_tx_start_fn start, void* context) {
    if (*busy) {
        return false;
    }

    *busy = true;
    if (start(context, 0U, length)) {
        return true;
    }

    *busy = false;
    return false;
}

/** Reserve a response before starting USB, because completion may run before the start call returns. */
static inline bool dap_usb_response_start(volatile dap_usb_response_state_t* state) {
    if (*state != DAP_USB_RESPONSE_READY) {
        return false;
    }
    *state = DAP_USB_RESPONSE_IN_FLIGHT;
    return true;
}

static inline void dap_usb_response_retry(volatile dap_usb_response_state_t* state) {
    if (*state == DAP_USB_RESPONSE_IN_FLIGHT) {
        *state = DAP_USB_RESPONSE_READY;
    }
}

static inline void dap_usb_response_complete(volatile dap_usb_response_state_t* state) {
    if (*state == DAP_USB_RESPONSE_IN_FLIGHT) {
        *state = DAP_USB_RESPONSE_FREE;
    }
}

#endif
