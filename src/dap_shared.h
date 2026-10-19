/** Everything the two images share, in one struct at one address both link scripts give it.
 *
 * CPU 0 and CPU 1 are built and linked separately, so nothing passes between them by symbol. Each
 * image defines `dap_shared` in the `.dap_shared` section, and each link script places that section
 * at DAP_SHARED_ORIGIN from CMakeLists.txt. tools/image.py checks that both ELFs agree on its
 * address, its size and where every member sits.
 *
 * It lives in USB SRAM because the packet buffers are USB DMA targets and the HS controller reaches
 * no other memory. That RAM is Device memory to both cores, and CPU 1 has no MPU to change that, so
 * an unaligned access to anything here faults. CPU 1 runs each command from its own copy; see
 * src/core1_main.c.
 */
#ifndef DAP_SHARED_H
#define DAP_SHARED_H

#include <stddef.h>
#include <stdint.h>

#include "dap_usb_slots.h"
#include "reboot.h"
#include "swo_ring.h"

#ifndef REQUEST_SLOTS
#define REQUEST_SLOTS 3U
#endif

/** DAP_PACKET_SIZE and DAP_RESPONSE_SIZE, which CPU 1 checks against DAP_config.h. Repeated here
 *  so the host simulator can include this header without the device headers. */
#define DAP_SHARED_PACKET_SIZE 512U
#define DAP_SHARED_RESPONSE_SIZE (DAP_SHARED_PACKET_SIZE + 2U + (8U * 255U) + 4U)

#define DAP_SHARED_ALIGN __attribute__((aligned(DAP_USB_BUFFER_ALIGN)))

/** SWO capture's DMA descriptors, four words each: the ring's, then DMA0 channel 8's table entry.
 *  SRAMBASE points at the first, so the ring's sit in the table entries of channels 0 to 7, which
 *  nothing enables. A table starts on 512 bytes. */
#define DAP_SHARED_SWO_DMA_WORDS ((SWO_SEGMENTS + 1U) * 4U)
#define DAP_SHARED_SWO_DMA_ALIGN __attribute__((aligned(512)))

/** The HID report size, which CPU 0 checks against CFG_TUD_HID_EP_BUFSIZE. */
#define DAP_SHARED_HID_REPORT_SIZE 64U

/** USB control request (vendor, device to host) for dap_core1_status_t, or with wValue
 *  DAP_SHARED_QUEUE_STATUS for the queues' state. CPU 0 answers it, so it works whatever state
 *  CPU 1 is in. */
#define DAP_SHARED_CORE1_STATUS_REQUEST 0x04U
#define DAP_SHARED_QUEUE_STATUS 1U
/** With wValue DAP_SHARED_SWO_STATUS: SWO capture's hardware as CPU 0 reads it (src/usb_descriptors.c). */
#define DAP_SHARED_SWO_STATUS 2U

/** USB control request (vendor, host to device): stop CPU 1 where it is and record it. CPU 0
 *  routes a mailbox interrupt to CPU 1's NMI and raises it, so CPU 1's fault record (above) gets
 *  the interrupted PC however CPU 1 was masked. CPU 1 stays stopped until the probe resets. */
#define DAP_SHARED_CORE1_SAMPLE_REQUEST 0x05U

/** CPU 1's progress and its last fault, for CPU 0 to report. */
typedef struct {
    uint32_t commands;   // replies published
    uint32_t command;    // the first four bytes of the command running, or of the last one
    uint32_t exception;  // IPSR of the fault CPU 1 stopped in; 0 while it runs. Written last.
    uint32_t cfsr;
    uint32_t hfsr;
    uint32_t bfar;
    uint32_t pc;  // from the fault's stacked frame
    uint32_t lr;
    uint32_t xpsr;
    uint32_t stale;  // commands from a host that had gone, dropped without running
} dap_core1_status_t;

/** Which transport response_transport names. */
enum {
    DAP_TRANSPORT_BULK,
    DAP_TRANSPORT_HID,
};

/** Three rings of REQUEST_SLOTS, each handed over through its `_len` array (dap_usb_slot_free).
 *
 * A host keeps at most DAP_PACKET_COUNT commands outstanding, and that is REQUEST_SLOTS. So with
 * a reply slot per command, CPU 1 can run every command it has been sent without waiting for an
 * earlier reply to leave, and a conforming HID host never finds its ring full.
 */
typedef struct {
    // Bulk requests: CPU 0 fills, CPU 1 releases as soon as it has copied the command.
    volatile uint16_t request_len[REQUEST_SLOTS];
    volatile uint32_t request_seq[REQUEST_SLOTS];
    volatile uint8_t request_head;
    volatile uint8_t request_tail;
    volatile uint8_t rx_armed_slot;
    volatile uint32_t next_request_seq;
    // Where this USB configuration's numbering starts. Commands before it, and their replies, are
    // a gone host's, and both cores drop them (src/main.c, tud_mount_cb).
    volatile uint32_t first_live_seq;
    // HID requests, the same way. CPU 0 copies each report in, because TinyUSB's HID class receives
    // into a buffer of its own.
    volatile uint16_t hid_request_len[REQUEST_SLOTS];
    volatile uint32_t hid_request_seq[REQUEST_SLOTS];
    volatile uint8_t hid_request_head;
    volatile uint8_t hid_request_tail;
    // Replies, in the order CPU 1 ran their commands: CPU 1 fills, CPU 0 releases once USB has sent
    // the slot. Both transports share the ring and CPU 0 sends from its head only.
    volatile uint16_t response_len[REQUEST_SLOTS];
    volatile uint32_t response_seq[REQUEST_SLOTS];  // the command's
    volatile uint8_t response_transport[REQUEST_SLOTS];
    volatile uint8_t response_head;
    volatile uint8_t response_tail;

    /** DAP_TransferAbort lives in CPU 1's RAM. CPU 1 publishes its address before it runs a
     *  command; CPU 0 sets it the moment an abort arrives. */
    volatile uint8_t* volatile transfer_abort;

    /** A reboot ID_VENDOR_REBOOT asked for, published only once its reply is READY. */
    volatile uint8_t reboot_mode;

    /** The LED's inputs, written by CPU 1 and read by CPU 0's LED tick. */
    volatile uint8_t led_activity;
    volatile uint8_t led_connected;

    /** SWO ring segments DMA0 has completed, counted by CPU 0's DMA interrupt for src/swo.c. */
    volatile uint32_t swo_segments;

    volatile dap_core1_status_t core1;

    DAP_SHARED_ALIGN uint8_t request[REQUEST_SLOTS][DAP_SHARED_PACKET_SIZE];
    // A reply is never more than a packet: CPU 1 builds it in its own buffer and copies a packet out.
    DAP_SHARED_ALIGN uint8_t response[REQUEST_SLOTS][DAP_SHARED_PACKET_SIZE];
    DAP_SHARED_ALIGN uint8_t hid_request[REQUEST_SLOTS][DAP_SHARED_HID_REPORT_SIZE];

    // In USB SRAM so that a streaming endpoint can send trace from where DMA0 wrote it.
    DAP_SHARED_SWO_DMA_ALIGN volatile uint32_t swo_dma[DAP_SHARED_SWO_DMA_WORDS];
    DAP_SHARED_ALIGN volatile uint8_t swo_ring[SWO_RING_BYTES];
} dap_shared_t;

extern dap_shared_t dap_shared;

/** Clear the hand-off state. USB SRAM is not cleared by either startup. */
static inline void dap_shared_init(volatile dap_shared_t* state) {
    volatile uint8_t* bytes = (volatile uint8_t*)state;
    for (size_t i = 0U; i < offsetof(dap_shared_t, request); i++) {
        bytes[i] = 0U;
    }
    state->reboot_mode = REBOOT_NONE;
}

#endif
