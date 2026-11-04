#include "aux.h"

#include <string.h>

#include "fsl_device_registers.h"
#include "tusb.h"

#include "dap_core.h"
#include "dap_shared.h"
#include "dio_edges.h"
#include "energy_stream.h"

// DAP_config.h first: DAP.h reads the settings it defines.
#include "DAP_config.h"

#include "DAP.h"
#include "usb_speed.h"

/** USB1 reaches no memory but USB SRAM. Every message fits one high-speed packet, so one buffer
 *  each way; at full speed a stream message spans several. */
CFG_TUD_MEM_SECTION CFG_TUSB_MEM_ALIGN static uint8_t aux_out[512];
CFG_TUD_MEM_SECTION CFG_TUSB_MEM_ALIGN static uint8_t aux_in[512];

static bool present;
static uint8_t instance;
/** OUT is armed into aux_out. */
static volatile bool out_armed;
/** aux_in holds a reply or a stream message of this length, to send or on its way while in_busy. */
static volatile uint16_t in_length;
static volatile bool in_busy;
/** The bulk packet size of this configuration. */
static uint16_t in_packet = USB_BULK_HIGH_SPEED;
/** The message on its way is a whole number of packets, so nothing marks its end: a zero-length
 *  packet follows it. Only at full speed, where a 48-pair message is 256 bytes; at high speed every
 *  message is shorter than a packet. */
static volatile bool zlp_due;
/** Counts USB configurations, for the slot's `generation`. */
static uint32_t generation;
/** A stream command (0x90 mode 6, 7, 10 or 13) CPU 0 answers itself, waiting for the capture
 *  state. */
static uint8_t local_request[26];
static uint8_t local_length;
/** A header has gone since this configuration began: stream messages before it are a gone host's.
 *  Only a header this configuration asked for, by mode 5, 6, 9 or 10, counts. */
static bool stream_synced;
static bool stream_expected;

/** Out of line, as aux_task is, so tools/usb_checks.py can call it. */
__attribute__((noinline)) void aux_init(bool has_aux, uint8_t vendor_index) {
    present = has_aux;
    instance = vendor_index;
}

__attribute__((section(".ramfunc"))) bool aux_owns(uint8_t vendor_index) {
    return present && (vendor_index == instance);
}

__attribute__((section(".ramfunc"))) void aux_received(const uint8_t* buffer, uint32_t length) {
    out_armed = false;
    aux_slot_t* const slot = &dap_stage.aux;
    // aux_task arms OUT only while the slot is free; the state check keeps anything else from
    // overwriting a request CPU 1 holds. It arms OUT again either way.
    if ((buffer != aux_out) || (length == 0U) || (slot->state != AUX_SLOT_FREE)) {
        return;
    }
    // Modes 6, 7, 10 and 13 start and stop a stream without stopping the capture. CPU 0 owns the
    // capture state whenever CPU 1 has not asked for quiet (src/energy_integration.h), so it serves
    // them.
    if ((buffer[0] == 0x90U) && (length >= 2U) &&
        ((buffer[1] == 6U) || (buffer[1] == 7U) || (buffer[1] == 10U) || (buffer[1] == 13U))) {
        local_length = (uint8_t)(length < sizeof(local_request) ? length : sizeof(local_request));
        memcpy(local_request, buffer, local_length);
        return;
    }
    uint32_t const kept = length < AUX_REQUEST_MAX ? length : AUX_REQUEST_MAX;
    // Zero past what arrived: CPU 1 checks the length, but a handler can still read a byte beyond it.
    // Volatile word stores, which no compiler turns into a call to the library's memset in flash.
    volatile uint32_t* const words = (volatile uint32_t*)(void*)slot->request;
    for (uint32_t i = 0U; i < AUX_REQUEST_MAX / 4U; i++) {
        words[i] = 0U;
    }
    memcpy(slot->request, buffer, kept);
    slot->request_len = (uint8_t)kept;
    slot->generation = generation;
    __DMB();
    slot->state = AUX_SLOT_REQUEST;
    __DMB();
    MAILBOX->MBOXIRQ[DAP_CORE_MAILBOX_CPU1].IRQSET = DAP_CORE_MAILBOX_WAKE_MASK;
    __DSB();
}

__attribute__((section(".ramfunc"))) void aux_sent(void) {
    if (zlp_due) {
        zlp_due = false;
        // in_busy holds until this completes. If it cannot start, the interface has gone, and the
        // next configuration's aux_mount clears in_busy.
        (void)tud_vendor_n_write_xfer(instance, aux_in, 0U);
        return;
    }
    in_busy = false;
    in_length = 0U;
    aux_task();
}

__attribute__((section(".ramfunc"))) void aux_mount(void) {
    generation++;
    out_armed = false;
    in_busy = false;
    in_length = 0U;
    in_packet = usb_bulk_packet_size();
    zlp_due = false;
    local_length = 0U;
    stream_synced = false;
    stream_expected = false;
}

/** Whether CPU 1 holds the capture state: it asked for quiet, or a start has not yet given its
 *  timeline. */
__attribute__((section(".ramfunc"))) static bool stream_owned_by_cpu1(struct energy_stream const* stream) {
    return ((dap_shared.energy_control_ack & DAP_ENERGY_QUIET_FLAG) != 0U) || stream->timeline_pending;
}

/** Whether a mode 6, 7, 10 or 13 has to wait for the capture state. CPU 1 holds it from boot to
 *  the first start and after every stop, with nothing running and no stream, and then the answer
 *  needs no write. It also holds it for a moment inside a command, around a supply read, with the
 *  capture running: then the answer waits. */
__attribute__((section(".ramfunc"))) static bool stream_waits(void) {
    struct energy_integration const* const energy = dap_shared.energy_integration;
    if ((energy != nullptr) && (local_request[1] == 13U) && ((energy->capture_id_sequence & 1U) != 0U)) {
        return true;
    }
    bool const failed = ((local_request[1] == 6U) || (local_request[1] == 10U) || (local_request[1] == 13U)) &&
                        (dap_shared.energy_dma_error != 0U);
    return (energy != nullptr) && (dap_shared.energy_available != 0U) && !failed &&
           stream_owned_by_cpu1(&energy->stream) && (energy->running || energy->stream.active);
}

/** 0x90 mode 6, 7, 10 or 13 into aux_in; false while it has to wait (stream_waits). Replies as
 *  HOST-INTERFACE.md "The energy stream" gives them. */
__attribute__((section(".ramfunc"))) static bool stream_command(void) {
    if (stream_waits()) {
        return false;
    }
    __DMB();
    bool const identified = (local_request[1] == 10U) || (local_request[1] == 13U);
    bool const start = (local_request[1] == 6U) || (local_request[1] == 10U);
    aux_in[0] = 0x90U;
    aux_in[1] = DAP_OK;
    if ((start && (local_length < 3U)) || (identified && !start && (local_length < 26U))) {
        aux_in[0] = ID_DAP_Invalid;
        in_length = 1U;
        return true;
    }
    uint8_t result = 0U;
    // The DMA interrupt is the only other owner on this core.
    uint32_t const basepri = cpu0_mask_all_but_edges();
    struct energy_integration* const energy = dap_shared.energy_integration;
    struct energy_stream* const stream = (energy != nullptr) ? &energy->stream : nullptr;
    if ((stream == nullptr) || (dap_shared.energy_available == 0U)) {
        result = 3U;
    } else if (identified && !start) {
        uint32_t const before = energy->capture_id_sequence;
        __DMB();
        energy_capture_id_t const capture_id = energy->capture_id;
        __DMB();
        if (((before & 1U) != 0U) || (before != energy->capture_id_sequence)) {
            cpu0_unmask(basepri);
            return false;
        }
        if (!energy_identity_matches(&capture_id, local_request + 2U)) {
            result = 8U;
        } else if (!stream_owned_by_cpu1(stream)) {
            energy_stream_end(stream, ENERGY_STREAM_END_ASKED, energy->decoder.sequence);
        }
    } else if (!start) {
        // Owned by CPU 1 means nothing streams (stream_waits).
        if (!stream_owned_by_cpu1(stream)) {
            energy_stream_end(stream, ENERGY_STREAM_END_ASKED, energy->decoder.sequence);
        }
    } else if (dap_shared.energy_dma_error != 0U) {
        result = 7U;
    } else if (stream_owned_by_cpu1(stream) || !energy->running) {
        result = 1U;
    } else {
        energy_stream_end(stream, ENERGY_STREAM_END_ASKED, energy->decoder.sequence);
        if (identified) {
            stream->sent = stream->written;
        }
        uint8_t levels = 0U;
        uint32_t stamp = 0U;
        uint8_t const pins = dio_edges_pins(local_request[2], &levels, &stamp);
        energy_stream_header_t const header = {
            .rate_hz = stream->clock_hz / stream->period,
            .setting = energy->setting,
            .full_scale_mv = (uint16_t)energy->full_scale_mv,
            .first_sequence = stream->next_sequence,
            .previous_low = energy->previous_low,
            .low = &energy->low_range,
            .high = &energy->high_range,
            .dio_mask = pins,
            .dio_levels = levels,
            .dio_stamp = stamp,
            .edge_resolution_ns = (pins != 0U) ? energy_stream_edge_resolution(stream->clock_hz) : 0U,
            .capture_id = identified ? &energy->capture_id : nullptr,
            .switch_weight_q16 = energy->switch_weight_q16,
            .rearm_between_pairs = energy->rearm_between_pairs,
            .previous_known = energy->previous_known,
            .previous_na = energy->previous_na,
            .high_range_only = energy->high_range_only,
        };
        // A full ring: the host is not reading, and the header would be lost too.
        result = energy_stream_begin(stream, &header) ? 0U : 7U;
        stream_expected = stream_expected || (result == 0U);
        if (identified && (result == 0U)) {
            stream_synced = false;
        }
    }
    cpu0_unmask(basepri);
    aux_in[2] = result;
    in_length = 3U;
    return true;
}

/** A capture failure ends its stream wherever the failure is found, but some leave no owner to
 *  write the end: a DMA error with no completion behind it, a pause or a start whose handover
 *  timed out, which leaves the state with CPU 0 (capture_quiet in src/energy_monitor.c). CPU 0's
 *  loop ends it then. */
__attribute__((section(".ramfunc"))) static void stream_fault(void) {
    struct energy_integration* const energy = dap_shared.energy_integration;
    if ((energy == nullptr) || !energy->stream.active || (dap_shared.energy_dma_error == 0U) ||
        ((dap_shared.energy_control_ack & DAP_ENERGY_QUIET_FLAG) != 0U)) {
        return;
    }
    uint32_t const basepri = cpu0_mask_all_but_edges();
    // A start that never gave its timeline gives its edges no times either.
    if (energy->stream.timeline_pending) {
        energy->stream.edge_mask = 0U;
    }
    energy_stream_end(&energy->stream, ENERGY_STREAM_END_FAULT, energy->decoder.sequence);
    cpu0_unmask(basepri);
}

/** Markers go with the next pairs message, but no later than 10 ms after the first of them: a
 *  capture below about 100 pairs a second takes longer than that to send one. Sending them sends the
 *  pairs message being built first, as edges do. Only while CPU 0 owns the stream's state; CPU 1
 *  sends them at its own closes. */
__attribute__((section(".ramfunc"), noinline)) static bool marks_due(void) {
    struct energy_integration const* const energy = dap_shared.energy_integration;
    return (energy != nullptr) && !stream_owned_by_cpu1(&energy->stream) &&
           energy_stream_marks_due(&energy->stream, &CTIMER4->TC);
}

__attribute__((section(".ramfunc"), noinline)) static void stream_marks(void) {
    if (!marks_due()) {
        return;
    }
    uint32_t const basepri = cpu0_mask_all_but_edges();
    struct energy_integration* const energy = dap_shared.energy_integration;
    // The DMA interrupt may have sent them, or ended the stream, since the check.
    if (!stream_owned_by_cpu1(&energy->stream) && energy->stream.active) {
        energy_stream_close(&energy->stream);
    }
    cpu0_unmask(basepri);
}

/** Stream messages wait while a mode 5 or 9 request is out, so its reply comes before its header. */
__attribute__((section(".ramfunc"))) static bool stream_held(void) {
    aux_slot_t const* const slot = &dap_stage.aux;
    return (slot->state != AUX_SLOT_FREE) && (slot->request[0] == 0x90U) && ((slot->request[1] == 5U) || (slot->request[1] == 9U));
}

/** The next stream message into aux_in. Before a header since the configuration began, messages
 *  are a gone host's and dropped; aux_task drops any taken while unconfigured. */
__attribute__((section(".ramfunc"))) static void stream_take(void) {
    struct energy_integration* const energy = dap_shared.energy_integration;
    if ((energy == nullptr) || stream_held()) {
        return;
    }
    struct energy_stream* const stream = &energy->stream;
    while (stream->sent != stream->written) {
        __DMB();
        uint32_t const at = stream->sent & (ENERGY_STREAM_SLOTS - 1U);
        uint8_t const* const message = energy_stream_slots[at];
        if (stream_synced || ((message[0] == ENERGY_STREAM_HEADER) && stream_expected)) {
            stream_synced = true;
            stream_expected = false;
            memcpy(aux_in, message, stream->length[at]);
            in_length = stream->length[at];
        }
        __DMB();
        stream->sent++;
        if (in_length != 0U) {
            return;
        }
    }
}

__attribute__((section(".ramfunc"), noinline)) void aux_task(void) {
    if (!present) {
        return;
    }
    aux_slot_t* const slot = &dap_stage.aux;
    // Requests arrive only while the host has the interface configured, and a configuration moves
    // `generation`, so a reply while it is not configured is a gone host's.
    bool const configured = tud_vendor_n_mounted(instance);
    if ((in_length == 0U) && (local_length != 0U) && stream_command()) {
        local_length = 0U;
    }
    if ((in_length == 0U) && (slot->state == AUX_SLOT_REPLY)) {
        __DMB();
        uint8_t const length = slot->reply_len;
        bool const live = slot->generation == generation;
        // Mode 5's header is in the ring already, held back until this reply has gone.
        if (live && (slot->request[0] == 0x90U) && ((slot->request[1] == 5U) || (slot->request[1] == 9U))) {
            stream_expected = true;
            if ((slot->request[1] == 9U) && (slot->reply_len >= 3U) && (slot->reply[2] == 0U)) {
                stream_synced = false;
            }
        }
        if (live) {
            memcpy(aux_in, slot->reply, length);
        }
        __DMB();
        slot->state = AUX_SLOT_FREE;
        in_length = live ? length : 0U;
    }
    stream_fault();
    stream_marks();
    if (in_length == 0U) {
        stream_take();
    }
    if ((in_length != 0U) && !in_busy) {
        if (!configured) {
            in_length = 0U;
        } else {
            in_busy = true;
            zlp_due = (in_length & (in_packet - 1U)) == 0U;
            if (tud_vendor_n_write_xfer(instance, aux_in, in_length) != in_length) {
                in_busy = false;
                zlp_due = false;
            }
        }
    }
    if (!out_armed && (slot->state == AUX_SLOT_FREE) && (local_length == 0U) && configured) {
        out_armed = true;
        if (!tud_vendor_n_read_xfer_buf(instance, aux_out)) {
            out_armed = false;
        }
    }
}

/** Out of line, so tools/usb_checks.py can call it. */
__attribute__((section(".ramfunc"), noinline)) bool aux_has_work(void) {
    if (!present) {
        return false;
    }
    if (in_length != 0U) {
        return !in_busy;
    }
    struct energy_integration const* const energy = dap_shared.energy_integration;
    bool const messages = (energy != nullptr) && (energy->stream.sent != energy->stream.written) && !stream_held();
    bool const fault = (energy != nullptr) && energy->stream.active && (dap_shared.energy_dma_error != 0U) &&
                       ((dap_shared.energy_control_ack & DAP_ENERGY_QUIET_FLAG) == 0U);
    return (dap_stage.aux.state == AUX_SLOT_REPLY) || messages || fault || marks_due() ||
           ((local_length != 0U) && !stream_waits());
}
