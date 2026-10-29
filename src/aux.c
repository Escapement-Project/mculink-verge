#include "aux.h"

#include <string.h>

#include "fsl_device_registers.h"
#include "tusb.h"

#include "dap_core.h"
#include "dap_shared.h"

/** USB1 reaches no memory but USB SRAM. Every message fits one packet, so one buffer each way. */
CFG_TUD_MEM_SECTION CFG_TUSB_MEM_ALIGN static uint8_t aux_out[512];
CFG_TUD_MEM_SECTION CFG_TUSB_MEM_ALIGN static uint8_t aux_in[512];

static bool present;
static uint8_t instance;
/** OUT is armed into aux_out. */
static volatile bool out_armed;
/** aux_in holds a reply of this length, to send or on its way while in_busy. */
static volatile uint8_t in_length;
static volatile bool in_busy;
/** Counts USB configurations, for the slot's `generation`. */
static uint32_t generation;

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
    uint32_t const kept = length < AUX_REQUEST_MAX ? length : AUX_REQUEST_MAX;
    // Zero past what arrived: CPU 1 checks the length, but a handler can still read a byte beyond it.
    uint32_t words[AUX_REQUEST_MAX / 4U] = {0};
    memcpy(words, buffer, kept);
    memcpy(slot->request, words, AUX_REQUEST_MAX);
    slot->request_len = (uint8_t)kept;
    slot->generation = generation;
    __DMB();
    slot->state = AUX_SLOT_REQUEST;
    __DMB();
    MAILBOX->MBOXIRQ[DAP_CORE_MAILBOX_CPU1].IRQSET = DAP_CORE_MAILBOX_WAKE_MASK;
    __DSB();
}

__attribute__((section(".ramfunc"))) void aux_sent(void) {
    in_busy = false;
    in_length = 0U;
    aux_task();
}

__attribute__((section(".ramfunc"))) void aux_mount(void) {
    generation++;
    out_armed = false;
    in_busy = false;
    in_length = 0U;
}

__attribute__((section(".ramfunc"), noinline)) void aux_task(void) {
    if (!present) {
        return;
    }
    aux_slot_t* const slot = &dap_stage.aux;
    // Requests arrive only while the host has the interface configured, and a configuration moves
    // `generation`, so a reply while it is not configured is a gone host's.
    bool const configured = tud_vendor_n_mounted(instance);
    if ((in_length == 0U) && (slot->state == AUX_SLOT_REPLY)) {
        __DMB();
        uint8_t const length = slot->reply_len;
        bool const live = slot->generation == generation;
        if (live) {
            memcpy(aux_in, slot->reply, length);
        }
        __DMB();
        slot->state = AUX_SLOT_FREE;
        in_length = live ? length : 0U;
    }
    if ((in_length != 0U) && !in_busy) {
        if (!configured) {
            in_length = 0U;
        } else {
            in_busy = true;
            if (tud_vendor_n_write_xfer(instance, aux_in, in_length) != in_length) {
                in_busy = false;
            }
        }
    }
    if (!out_armed && (slot->state == AUX_SLOT_FREE) && configured) {
        out_armed = true;
        if (!tud_vendor_n_read_xfer_buf(instance, aux_out)) {
            out_armed = false;
        }
    }
}

/** Out of line, so tools/usb_checks.py can call it. */
__attribute__((section(".ramfunc"), noinline)) bool aux_has_work(void) {
    return present && (((in_length == 0U) && (dap_stage.aux.state == AUX_SLOT_REPLY)) ||
                       ((in_length != 0U) && !in_busy));
}
