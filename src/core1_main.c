/** CPU 1: execute the DAP commands CPU 0 receives, one at a time, and publish each reply.
 *
 * This is its own image, built for CPU 1's core, which has no FPU, no DSP extension and no MPU.
 * core1_start.S copies .data and clears .bss before core1_main runs. CPU 1 takes no interrupts:
 * it polls the mailbox for CPU 0's signal.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "fsl_device_registers.h"

#include "DAP_config.h"
#include "DAP.h"
#include "board_pins.h"
#include "dap_core.h"
#include "dap_dispatch.h"
#include "dap_hid.h"
#include "dap_shared.h"
#include "dio.h"
#include "energy_monitor.h"
#include "hot.h"
#include "led.h"
#include "target_power.h"
#include "target_sense.h"

static_assert(DAP_PACKET_SIZE == DAP_SHARED_PACKET_SIZE, "dap_shared.h repeats DAP_PACKET_SIZE");

dap_shared_t dap_shared __attribute__((section(".dap_shared")));
dap_stage_t dap_stage __attribute__((section(".dap_stage")));

/** Every HID command's reply, in CPU 1's own SRAM, a packet long: the command layer bounds its
 *  writes by the packet size, and only the report goes out to dap_stage. Every other command and
 *  reply runs in dap_stage (src/dap_shared.h), where CPU 0 copies it in and out.
 */
alignas(DAP_USB_BUFFER_ALIGN) static uint8_t response_copy[DAP_PACKET_SIZE];

/** The same buffer, read through volatile so link-time optimisation cannot specialise
 *  DAP_ExecuteCommandSized to it. tools/cycles.py and tools/image.py call it with buffers of their
 *  own, and what they check has to be the code that ships. */
static uint8_t* const volatile response_buffer = response_copy;

/** CPU 0 waits for work off flash (src/main.c), and this is how it hears of CPU 1's. */
static void wake_core0(void) {
    __DMB();
    MAILBOX->MBOXIRQ[DAP_CORE_MAILBOX_CPU0].IRQSET = DAP_CORE_MAILBOX_WAKE_MASK;
}

/** Answer the Verge Aux request in dap_stage.aux (src/aux_slot.h), with the dispatch and size checks
 *  a CMSIS-DAP command gets. Out of the command path: it runs once a request, between commands. */
__attribute__((noinline)) static void aux_serve(void) {
    aux_slot_t* const slot = &dap_stage.aux;
    __DMB();
    uint32_t produced = 0U;
    if (aux_command_carried(slot->request[0])) {
        dap_vendor_on_aux = true;
        produced = DAP_ExecuteCommandSized(slot->request, slot->request_len, AUX_REPLY_MAX, slot->reply) & 0xFFFFU;
        dap_vendor_on_aux = false;
    }
    if ((produced == 0U) || (produced > AUX_REPLY_MAX)) {
        slot->reply[0] = ID_DAP_Invalid;
        produced = 1U;
    }
    slot->reply_len = (uint8_t)produced;
    __DMB();
    slot->state = AUX_SLOT_REPLY;
    wake_core0();
}

COMMAND_PATH static bool dap_task(void) {
    // Ahead of the queue, so an aux request waits for the command running and no other.
    if (dap_stage.aux.state == AUX_SLOT_REQUEST) {
        aux_serve();
        return true;
    }
    __DMB();
    uint8_t const bulk = dap_shared.request_head;
    uint8_t const hid = dap_shared.hid_request_head;
    uint8_t const bulk_out = dap_shared.response_tail;
    uint8_t const hid_out = dap_shared.hid_response_tail;
    bool const bulk_ready = dap_core_ready(!dap_usb_slot_free(dap_shared.request_len, bulk),
                                          dap_usb_slot_free(dap_shared.response_len, bulk_out));
    bool const hid_ready = dap_core_ready(!dap_usb_slot_free(dap_shared.hid_request_len, hid),
                                         dap_usb_slot_free(dap_shared.hid_response_len, hid_out));
    if (!dap_core_has_work(bulk_ready, hid_ready)) {
        return false;
    }
    __DMB();

    bool const use_hid =
        dap_usb_choose_hid(hid_ready, dap_shared.hid_request_seq[hid], bulk_ready, dap_shared.request_seq[bulk]);
    uint32_t const seq = use_hid ? dap_shared.hid_request_seq[hid] : dap_shared.request_seq[bulk];
    uint8_t const stage = use_hid ? dap_shared.hid_request_stage[hid] : dap_shared.request_stage[bulk];
    uint16_t const request_length = use_hid ? dap_shared.hid_request_len[hid] : dap_shared.request_len[bulk];
    uint32_t const packet_size = use_hid ? DAP_SHARED_HID_REPORT_SIZE : dap_shared.bulk_packet_size;
    uint8_t* const request = use_hid ? dap_stage.hid_request[stage] : dap_stage.request[stage];
    uint8_t* const response = use_hid ? response_buffer : dap_stage.response[bulk_out];

    // The slot goes back to CPU 0, which can receive the next command into it while this one runs.
    __DMB();
    if (use_hid) {
        dap_shared.hid_request_head = dap_usb_slot_release(dap_shared.hid_request_len, hid, REQUEST_SLOTS);
    } else {
        dap_shared.request_head = dap_usb_slot_release(dap_shared.request_len, bulk, REQUEST_SLOTS);
        wake_core0();  // to receive into the slot
    }

    // The last thing before running it, to leave the abort the least room: a gone host's transfer
    // clears the flag as it starts, and then runs to its end.
    if (dap_usb_stale(seq, dap_shared.first_live_seq)) {
        dap_shared.core1.stale++;
        return true;
    }

    uint32_t command;
    memcpy(&command, request, sizeof(command));
    dap_shared.core1.command = command;

    led_activity();
    uint32_t produced = DAP_ExecuteCommandSized(request, request_length, packet_size, response) & 0xFFFFU;
    if (produced > packet_size) {
        produced = packet_size;
    }

    if (use_hid) {
        produced = dap_hid_reply(response, produced);
    }
    if ((produced != 0U) && use_hid) {
        memcpy(dap_stage.hid_response[hid_out], response_copy, produced);
        dap_shared.hid_response_seq[hid_out] = seq;
        __DMB();
        dap_shared.hid_response_len[hid_out] = (uint16_t)produced;
        dap_shared.hid_response_tail = dap_usb_next_slot(hid_out, REQUEST_SLOTS);
        wake_core0();
    } else if (produced != 0U) {
        dap_shared.response_seq[bulk_out] = seq;
        __DMB();
        dap_shared.response_len[bulk_out] = (uint16_t)produced;
        dap_shared.response_tail = dap_usb_next_slot(bulk_out, REQUEST_SLOTS);
        wake_core0();
    }
    dap_shared.core1.commands++;
    return true;
}

/** Every exception CPU 1 takes is a fault: it runs with interrupts off. Record it where CPU 0 can
 *  report it over USB, then stop. core1_start.S passes the stacked LR, return address and XPSR. */
[[noreturn]] __attribute__((used)) void core1_fault(uint32_t lr, uint32_t pc, uint32_t xpsr);
[[noreturn]] __attribute__((used)) void core1_fault(uint32_t lr, uint32_t pc, uint32_t xpsr) {
    dap_shared.core1.cfsr = SCB->CFSR;
    dap_shared.core1.hfsr = SCB->HFSR;
    dap_shared.core1.bfar = SCB->BFAR;
    dap_shared.core1.lr = lr;
    dap_shared.core1.pc = pc;
    dap_shared.core1.xpsr = xpsr;
    __DMB();
    dap_shared.core1.exception = __get_IPSR();
    for (;;) {
    }
}

/** Wait until CPU 0 raises CPU 1's mailbox register, polling its level from SRAM.
 *
 * CPU 1 used to sleep in WFE, woken by the mailbox interrupt becoming pending (SEVONPEND, the
 * interrupt disabled). During SWD downloads at 37.5 MHz and above it sometimes slept through: the
 * mailbox raised, three commands queued, CPU 1 in WFE, and the registers an NMI stacked there showed
 * its last check had found nothing. Why the pend made no event was not found. A level cannot be
 * missed. The interrupt stays disabled: the generated transfers clear PRIMASK.
 *
 * In SRAM after the transfer loops (src/sample_probe.c says why there), so the spin fetches nothing
 * from flash.
 */
__attribute__((noinline, section(".data_core1_wait"))) static void wait_for_mailbox(void) {
    while ((MAILBOX->MBOXIRQ[DAP_CORE_MAILBOX_CPU1].IRQ & DAP_CORE_MAILBOX_WAKE_MASK) == 0U) {
    }
}

/** Run each request CPU 0 publishes, and wait for the next. */
[[noreturn]] COMMAND_PATH static void serve(void) {
    for (;;) {
        if (dap_task()) {
            continue;
        }
        // Cleared before the check, so a raise CPU 0 makes after it still ends the wait.
        MAILBOX->MBOXIRQ[DAP_CORE_MAILBOX_CPU1].IRQCLR = DAP_CORE_MAILBOX_WAKE_MASK;
        __DSB();
        if (!dap_task()) {
            wait_for_mailbox();
        }
    }
}

/** Bring the DAP up, then wait for CPU 0 to publish another request. */
[[noreturn]] void core1_main(void) {
    board_timestamp_init();
    DAP_Setup();
    target_power_init();
    target_sense_init();
    energy_monitor_init();
    dio_init();
    dap_shared.transfer_abort = &DAP_TransferAbort;
    // Before the first command: calibration opens J9, and no session is under way yet. After
    // board_timestamp_init, since its settling waits count CYCCNT.
    energy_monitor_calibrate();
    serve();
}
