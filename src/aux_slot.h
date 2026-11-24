/** Verge Aux's request slot (docs/host-interface.md, "Verge Aux"), in dap_stage so CPU 1 runs the
 *  request in place, as it does a CMSIS-DAP command.
 *
 * CPU 0 puts each request the aux interface receives here, and CPU 1 answers it before its next
 * CMSIS-DAP command, with the code that answers the same command on CMSIS-DAP. One request at a
 * time: CPU 0 arms the aux OUT endpoint only while the slot is free.
 *
 * A request from a host that has since gone still runs; only its reply is dropped. Unlike a queued
 * CMSIS-DAP command, an aux command needs no session state from the host that sent it.
 */
#ifndef AUX_SLOT_H
#define AUX_SLOT_H

#include <stdalign.h>
#include <stdbool.h>
#include <stdint.h>

/** The most of a request CPU 0 keeps, zero-filled past what arrived. Longer than any aux command's:
 *  0x94 with a 32-byte label is the longest, 38 bytes. */
#define AUX_REQUEST_MAX 40U
/** The reply space CPU 1 gives each request: the HID report size, which 0x82's reply already fits. */
#define AUX_REPLY_MAX 64U

/** Each move has one writer: CPU 0 takes FREE to REQUEST and REPLY to FREE, CPU 1 REQUEST to REPLY.
 *  The writer fills the slot first and moves the state last. */
enum : uint8_t {
    AUX_SLOT_FREE,
    AUX_SLOT_REQUEST,
    AUX_SLOT_REPLY,
};

typedef struct {
    volatile uint8_t state;
    volatile uint8_t request_len;
    volatile uint8_t reply_len;
    /** CPU 0's count of USB configurations when the request arrived: a reply from before the
     *  latest is a gone host's, and CPU 0 drops it. */
    volatile uint32_t generation;
    alignas(4) uint8_t request[AUX_REQUEST_MAX];
    alignas(4) uint8_t reply[AUX_REPLY_MAX];
} aux_slot_t;

/** Identify firmware, the DIO pins, and the Pro's target supply, energy capture and markers.
 *  Anything else gets [0xff]. */
static inline bool aux_command_carried(uint8_t command) {
    return (command == 0x82U) || (command == 0x8EU) || (command == 0x8FU) || (command == 0x90U) ||
           (command == 0x94U);
}

#endif
