/** Everything the two images share, in one struct at one address both link scripts give it.
 *
 * CPU 0 and CPU 1 are built and linked separately, so nothing passes between them by symbol. Each
 * image defines `dap_shared` in the `.dap_shared` section, and each link script places that section
 * at DAP_SHARED_ORIGIN from CMakeLists.txt. tools/image.py checks that both ELFs agree on its
 * address, its size and where every member sits.
 *
 * It lives in USB SRAM because the packet buffers are USB DMA targets and the HS controller reaches
 * no other memory. That RAM is Device memory to both cores, and CPU 1 has no MPU to change that, so
 * an unaligned access to anything here faults. CPU 1 runs each command from a copy in Normal
 * memory: CPU 0's in dap_stage (below), or its own (src/core1_main.c).
 */
#ifndef DAP_SHARED_H
#define DAP_SHARED_H

#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>

#include "aux_slot.h"
#include "dap_usb_slots.h"
#include "dma_ring.h"
#include "energy_samples.h"
#include "energy_stream.h"
#include "swo_ring.h"

#ifndef REQUEST_SLOTS
#define REQUEST_SLOTS 3U
#endif

/** DAP_PACKET_SIZE, which CPU 1 checks against DAP_config.h. Repeated here
 *  so the host simulator can include this header without the device headers. */
#define DAP_SHARED_PACKET_SIZE 512U

#define DAP_SHARED_ALIGN alignas(DAP_USB_BUFFER_ALIGN)

/** DMA0's descriptor table, which SRAMBASE points at: an entry per channel through ADC0 FIFO0's
 *  channel 21. CPU 0 writes channels 4 to 7 for VCOM, and CPU 1 writes channels 8 and 21 for SWO
 *  and energy capture. A table starts on 512 bytes. */
#define DAP_SHARED_DMA_CHANNELS 22U
#define DAP_SHARED_DMA_TABLE_ALIGN alignas(512)
/** Descriptors start on 16 bytes. */
#define DAP_SHARED_DMA_DESCRIPTOR_ALIGN alignas(16)

/** dap_stage's request buffers for each transport: one more than the slots (see dap_stage_t). */
#define DAP_STAGE_REQUESTS (REQUEST_SLOTS + 1U)

/** The HID report size, which CPU 0 checks against CFG_TUD_HID_EP_BUFSIZE. */
#define DAP_SHARED_HID_REPORT_SIZE 64U

/** A segment's most words. Each capture picks its own size up to this: about 1 ms of pairs, which
 *  at 100,000 pairs/s is 512 words. XFERCOUNT would allow 1024. */
#define ENERGY_SEGMENT_MAX_WORDS 512U
#define ENERGY_SEGMENTS 2U
#define ENERGY_RING_WORDS (ENERGY_SEGMENT_MAX_WORDS * ENERGY_SEGMENTS)

/** Energy capture's integration state, in CPU 0's SRAM. src/energy_integration.h has the code
 *  both cores run on it, and who may change what. */
struct energy_integration {
    /** Set by CPU 1 for each capture, while CPU 0 has acknowledged quiet: copies of CPU 1's
     *  calibration and what the conversion needs from the start. */
    energy_calibration_t low_range;
    energy_calibration_t high_range;
    uint64_t scale_q32; // floor(2^32 x period / clock): a pair's share of a second
    uint32_t full_scale_mv;
    uint32_t rearm_below_na; // UM11673 Table 4's low-range limit for the setting
    uint32_t segments_at_start; // dap_shared.energy_segments when the ring started
    uint32_t segment_words; // a power of two, for the rate (src/energy_integration.h)
    uint8_t setting;        // the current range jumper, as a stream's header gives it: 1 350 mA, 2 50 mA
    bool calibrated;
    /** Half the share of a period the current's conversions span, in 1/65536 (src/energy_pairs.h). */
    uint32_t switch_weight_q16;
    /** Whether CPU 0 re-arms the low range between pairs, and when: CTIMER2 counts half periods of
     *  `trigger_ticks`, and a re-arm lands from `rearm_from_ticks` to `rearm_until_ticks` after a
     *  trigger (src/energy_integration.c). */
    bool rearm_between_pairs;
    uint32_t trigger_ticks;
    uint32_t rearm_from_ticks;
    uint32_t rearm_until_ticks;
    /** From a start to the end of the pause that follows: set and cleared by CPU 1 while CPU 0
     *  cannot take an energy completion. */
    bool running;

    /** Advanced by whichever core owns the capture. */
    uint32_t consumed_bytes;
    energy_sample_decoder_t decoder;
    /** What the samples since the last segment say about re-arming the low range: the last pair's
     *  range, a high-range pair at or over the limit, and one at or over `band_from_na`. */
    bool on_high_range;
    bool rearm_blocked;
    bool in_band;
    /** The re-arm's backoff (src/energy_pairs.h). In the band, from half the limit up to it, a
     *  target can reach the switch up on the low range alone, so that the range switches every few
     *  milliseconds (BOARDS.md). There a re-arm waits for `hold_segments` segments in a row
     *  under the limit. A low phase shorter than `short_low_pairs` after a re-arm from the band
     *  doubles it, up to `hold_max_segments`, if the pair `verdict_pairs` after the switch up still
     *  reads under the limit; one that lasts that long sets it back to 1. Set by a start. */
    uint32_t band_from_na;
    uint32_t short_low_pairs;
    uint32_t verdict_pairs;
    uint32_t hold_max_segments;
    uint32_t hold_segments;
    uint32_t clean_segments; // segments in a row under the limit, from the band, since the last re-arm
    uint32_t low_pairs;      // the low phase's pairs so far, up to short_low_pairs
    uint32_t verdict_in;     // pairs until a short low phase's verdict, 0 for none pending
    bool rearmed_in_band;    // the last re-arm asked for came from the band
    /** The capture holds the high range: U18's reset is held, so the latch never switches
     *  (`0x90` mode 14). Set by a new capture's start, before CPU 0 takes the state. */
    bool high_range_only;
    /** A high-range current code under this is too near the amplifier's floor to trust, whatever
     *  the range setting: the stream marks the pair under range. 0 marks none. */
    uint16_t under_range_code;
    /** A pair whose range differs from the one before may have converted its current across the
     *  switch, so it waits for the next pair's current (src/energy_pairs.h). A start that may use
     *  the low range sets the range before to low, because it clocks the latch to the low range. A
     *  down-switch between pairs does not wait. */
    bool previous_low;
    bool held;
    uint32_t held_mv;
    uint32_t held_na; // its own reading, for a pause that comes before the next pair
    /** The share of the pair before a held up-switch in its stand-in, in 1/65536; 0 after a down-switch
     *  or with no pair before it. */
    uint32_t held_weight_q16;
    uint32_t held_before_na;
    /** The last pair's own current through the sense resistor; none since a start or pause. */
    bool previous_known;
    uint32_t previous_na;

    /** Verge Aux's stream of the pairs integrated here (src/energy_stream.h). */
    struct energy_stream stream;
    volatile uint32_t capture_id_sequence;
    energy_capture_id_t capture_id;

    DAP_SHARED_DMA_DESCRIPTOR_ALIGN volatile dma_descriptor_t descriptors[ENERGY_SEGMENTS];
    volatile uint32_t ring[ENERGY_RING_WORDS];
};

#define DAP_ENERGY_QUIET_FLAG 1U

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


/** Four rings of REQUEST_SLOTS, each handed over through its `_len` array (dap_usb_slot_free): a
 *  request ring and a reply ring for each transport.
 *
 * A host keeps at most DAP_PACKET_COUNT commands outstanding, and that is REQUEST_SLOTS. So with
 * a reply slot per command, CPU 1 can run every command it has been sent without waiting for an
 * earlier reply to leave, and a conforming HID host never finds its ring full.
 *
 * Each transport has its own reply ring because a reply offered on an IN endpoint waits there until
 * a host reads it, and a host that has gone never will (Linux polls an HID endpoint only while its
 * node is open). In a shared ring that reply held the head, and the other interface's replies queued
 * behind it until no command could run.
 */
typedef struct {
    // Bulk requests: CPU 0 fills, CPU 1 releases as soon as it has taken the command.
    volatile uint16_t request_len[REQUEST_SLOTS];
    volatile uint32_t request_seq[REQUEST_SLOTS];
    volatile uint8_t request_stage[REQUEST_SLOTS];  // the dap_stage.request it is in
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
    volatile uint8_t hid_request_stage[REQUEST_SLOTS];
    volatile uint8_t hid_request_head;
    volatile uint8_t hid_request_tail;
    // Bulk replies, in the order CPU 1 ran their commands: CPU 1 fills, CPU 0 releases once USB has
    // sent the slot. CPU 0 sends from the head only.
    volatile uint16_t response_len[REQUEST_SLOTS];
    volatile uint32_t response_seq[REQUEST_SLOTS];  // the command's
    volatile uint8_t response_head;
    volatile uint8_t response_tail;
    // HID replies, the same way. Each is in dap_stage.hid_response, because TinyUSB's HID class sends
    // a copy from a buffer of its own.
    volatile uint16_t hid_response_len[REQUEST_SLOTS];
    volatile uint32_t hid_response_seq[REQUEST_SLOTS];
    volatile uint8_t hid_response_head;
    volatile uint8_t hid_response_tail;

    /** DAP_TransferAbort lives in CPU 1's RAM. CPU 1 publishes its address before it runs a
     *  command; CPU 0 sets it the moment an abort arrives. */
    volatile uint8_t* volatile transfer_abort;

    /** CMSIS-DAP v2's packet size at the speed the host chose: 512, or 64 at full speed. CPU 0
     *  sets it at each configuration, before a command of that configuration arrives. */
    volatile uint16_t bulk_packet_size;

    /** The LEDs' inputs, written by CPU 1 and read by CPU 0's LED tick. */
    volatile uint8_t led_activity;
    volatile uint8_t led_connected;
    volatile uint8_t led_wire;  // LED_WIRE_*
    volatile uint8_t led_swo;   // SWO bytes have gone to the host
    volatile uint8_t led_sio;   // the host drives a DIO pin (dio.h)

    /** Counted by CPU 1 as SPI takes the DIO pins and again as it gives them back, so odd while
     *  SPI has them. CPU 0 stops their edges meanwhile and marks the gap (src/dio_edges.c), from
     *  the stamping timer as CPU 1 read it just before it counted SPI taking them. */
    volatile uint32_t dio_spi_turns;
    volatile uint32_t dio_spi_stamp;
    /** The count CPU 0 last acted on. While it lags, the pins have been off since the first transfer
     *  CPU 0 hasn't seen, and CPU 1 keeps that transfer's stamp. */
    volatile uint32_t dio_spi_seen;

    /** BOARD_KIND_* and BOARD_JUMPER_* (board_straps.h), written by CPU 0 before CPU 1 starts. */
    volatile uint8_t board;
    volatile uint8_t jumpers;
    /** HW_VER_5 enables Pro power measurement, written with the board straps. */
    volatile uint8_t energy_available;
    /** The J8 pins the bench marker holds (src/marker.h), written by CPU 1: a DIO stream leaves them
     *  out. */
    volatile uint8_t dio_marker_pins;

    /** SWO ring segments DMA0 has completed, counted by CPU 0's DMA interrupt for src/swo.c. */
    volatile uint32_t swo_segments;
    /** Set by CPU 0's DMA interrupt when SWO's channel failed, which it clears; taken by CPU 1. */
    volatile uint8_t swo_dma_error;
    /** Set by CPU 0's DMA interrupt, before it clears the flags, when it finds SWO's count lost
     *  (dma_ring_take); CPU 1 restarts the ring and clears it. */
    volatile uint8_t swo_lost;
    /** Energy ring segments completed and its DMA error, maintained by CPU 0 for CPU 1. */
    volatile uint32_t energy_segments;
    /** Odd while CPU 0 clears an energy completion, integrates it and publishes its count. */
    volatile uint32_t energy_dma_sequence;
    volatile uint8_t energy_dma_error;
    /** CPU 0's, set before CPU 1 starts. */
    struct energy_integration* volatile energy_integration;
    /** The Pro range latch needs a pulse; CPU 0 defers it out of the DMA interrupt. */
    volatile uint8_t energy_range_retry_pending;
    volatile uint8_t energy_range_retry_enabled;
    /** Generation plus QUIET_FLAG; CPU 0 echoes the request from its main loop. */
    volatile uint32_t energy_control_request;
    volatile uint32_t energy_control_ack;
    /** CPU 0's energy accumulator, read by CPU 1 with the sequence counter. */
    volatile uint32_t energy_accumulator_sequence;
    volatile uint64_t energy_accumulator_pj;
    volatile uint64_t energy_accumulator_pairs;
    volatile uint32_t energy_accumulator_fraction_q32;

    volatile dap_core1_status_t core1;

    DAP_SHARED_ALIGN uint8_t request[REQUEST_SLOTS][DAP_SHARED_PACKET_SIZE];
    DAP_SHARED_ALIGN uint8_t response[REQUEST_SLOTS][DAP_SHARED_PACKET_SIZE];

    DAP_SHARED_DMA_TABLE_ALIGN volatile dma_descriptor_t dma_table[DAP_SHARED_DMA_CHANNELS];
    // SWO's ring, linked in a circle from its channel's table entry.
    DAP_SHARED_DMA_DESCRIPTOR_ALIGN volatile dma_descriptor_t swo_dma[SWO_SEGMENTS];
    // In USB SRAM so that a streaming endpoint can send trace from where DMA0 wrote it.
    DAP_SHARED_ALIGN volatile uint8_t swo_ring[SWO_RING_BYTES];
} dap_shared_t;

extern dap_shared_t dap_shared;

/** Commands and replies in Normal memory, where CPU 1 runs them in place. CPU 0 copies each
 *  command in from its USB slot as it arrives and each bulk reply out as it starts to send it,
 *  so CPU 1 copies nothing while commands are queued. Each image defines it in `.dap_stage`, which
 *  both link scripts place in the top 8 KiB of SRAM3 (CMakeLists.txt says why there). CPU 1 has the
 *  highest AHB matrix priority (src/main.c), so CPU 0's copies cannot delay the accesses a
 *  transfer makes to its buffers.
 *
 * CPU 1 frees a command's request slot as it takes the command, so a DAP_TransferAbort can still
 * arrive while the host has every slot in use. So the request buffers are not the slots: CPU 0
 * takes them in turn, one more of them than there are slots. A buffer comes round again after
 * REQUEST_SLOTS more commands, and CPU 0 cannot receive that many while the command in it runs.
 * HID has its own, a packet long, because the command layer bounds its reads by the packet size.
 * The reply buffers are the reply slots'. HID's hold one report each: every HID command runs in
 * CPU 1's own buffer, a packet long, and only its report is copied here, so no reply can run into
 * the next.
 */
typedef struct {
    DAP_SHARED_ALIGN uint8_t request[DAP_STAGE_REQUESTS][DAP_SHARED_PACKET_SIZE];
    DAP_SHARED_ALIGN uint8_t hid_request[DAP_STAGE_REQUESTS][DAP_SHARED_PACKET_SIZE];
    DAP_SHARED_ALIGN uint8_t response[REQUEST_SLOTS][DAP_SHARED_PACKET_SIZE];
    DAP_SHARED_ALIGN uint8_t hid_response[REQUEST_SLOTS][DAP_SHARED_HID_REPORT_SIZE];
    /** Verge Aux's one request and its reply (src/aux_slot.h). */
    aux_slot_t aux;
} dap_stage_t;

extern dap_stage_t dap_stage;

/** Clear the hand-off state, which neither startup does in USB SRAM or in dap_stage. CPU 0 calls it
 *  before it starts CPU 1. `energy` is CPU 0's integration state, which CPU 1 reaches only through
 *  the pointer set here. */
static inline void dap_shared_init(volatile dap_shared_t* state, dap_stage_t* stage, struct energy_integration* energy) {
    volatile uint8_t* bytes = (volatile uint8_t*)state;
    for (size_t i = 0U; i < offsetof(dap_shared_t, request); i++) {
        bytes[i] = 0U;
    }
    state->bulk_packet_size = DAP_SHARED_PACKET_SIZE;
    state->energy_integration = energy;
    stage->aux.state = AUX_SLOT_FREE;
}

#endif
