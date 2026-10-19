#ifndef DAP_USB_SLOTS_H
#define DAP_USB_SLOTS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DAP_USB_BUFFER_ALIGN 64U

typedef bool (*dap_usb_rx_start_fn)(void* context, uint8_t slot);
typedef bool (*dap_usb_tx_start_fn)(void* context, uint8_t slot, uint16_t length);

static inline uint8_t dap_usb_next_slot(uint8_t slot, uint8_t count) {
    return (uint8_t)((slot + 1U) % count);
}

static inline bool dap_usb_sequence_before(uint32_t left, uint32_t right) {
    return (int32_t)(left - right) < 0;
}

/** Whether the command numbered `seq`, or its reply, came from a host that has gone: before
 *  `first_live`, the number the current USB configuration started at. */
static inline bool dap_usb_stale(uint32_t seq, uint32_t first_live) {
    return dap_usb_sequence_before(seq, first_live);
}

/** A slot holds something while its length is non-zero. Whoever fills a slot writes the length
 *  last, and whoever empties it writes zero once it no longer needs the bytes. */
static inline bool dap_usb_slot_free(volatile uint16_t const* len, uint8_t slot) {
    return len[slot] == 0U;
}

static inline bool dap_usb_choose_hid(bool hid_ready, uint32_t hid_seq, bool bulk_ready, uint32_t bulk_seq) {
    return hid_ready && (!bulk_ready || dap_usb_sequence_before(hid_seq, bulk_seq));
}

static inline uint8_t dap_usb_slot_release(volatile uint16_t* len, uint8_t head, uint8_t count) {
    len[head] = 0U;
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
static inline bool dap_usb_start_tx(volatile bool* busy, uint8_t slot, uint16_t length,
                                    dap_usb_tx_start_fn start, void* context) {
    if (*busy) {
        return false;
    }

    *busy = true;
    if (start(context, slot, length)) {
        return true;
    }

    *busy = false;
    return false;
}

#endif
