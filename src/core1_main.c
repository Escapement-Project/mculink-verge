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
#ifdef DAP_TIMING_DIAGNOSTICS
#include "dap_timing.h"
#endif
#include "dio.h"
#include "energy_monitor.h"
#include "hot.h"
#include "led.h"
#include "marker.h"
#include "reboot.h"
#include "target_power.h"
#include "target_sense.h"

static_assert(DAP_PACKET_SIZE == DAP_SHARED_PACKET_SIZE, "dap_shared.h repeats DAP_PACKET_SIZE");

dap_shared_t dap_shared __attribute__((section(".dap_shared")));
dap_stage_t dap_stage __attribute__((section(".dap_stage")));

#ifdef MARKER_PINS
/** Which J8 pins the marker still owns. ID_VENDOR_J8 takes pins away from it and gives them back. */
volatile uint32_t marker_mask = J8_PIN_MASK;
#endif

/** The command being executed and its reply, in CPU 1's own SRAM, for a command CPU 0 did not
 *  stage.
 *
 * The generated transfer code loads and stores words at any offset in these: a block write's data
 * starts at byte 5, and inside DAP_ExecuteCommands a reply can start anywhere. Normal memory takes
 * that; USB SRAM is Device memory, where it faults. So each command is copied in from its USB slot
 * and its reply copied back out, both between word-aligned buffers: here by CPU 1, or by CPU 0
 * through dap_stage.
 */
alignas(DAP_USB_BUFFER_ALIGN) static uint8_t request_copy[DAP_PACKET_SIZE];
alignas(DAP_USB_BUFFER_ALIGN) static uint8_t response_copy[DAP_PACKET_SIZE];

/** The same two buffers, read through volatile so link-time optimisation cannot specialise
 *  DAP_ExecuteCommandSized to them. tools/cycles.py and tools/image.py call it with buffers of
 *  their own, and what they check has to be the code that ships. */
static uint8_t* const volatile request_buffer = request_copy;
static uint8_t* const volatile response_buffer = response_copy;

/** What ID_VENDOR_REBOOT asked for, held until the command's reply is READY. Published any
 *  earlier, CPU 0 would find no reply pending and reset before sending it. */
static uint8_t reboot_after_reply = REBOOT_NONE;

void reboot_request(uint8_t mode) {
    reboot_after_reply = mode;
}

/** CPU 0 waits for work off flash (src/main.c), and this is how it hears of CPU 1's. */
static void wake_core0(void) {
    __DMB();
    MAILBOX->MBOXIRQ[DAP_CORE_MAILBOX_CPU0].IRQSET = DAP_CORE_MAILBOX_WAKE_MASK;
}

COMMAND_PATH static bool dap_task(void) {
#ifdef DAP_TIMING_DIAGNOSTICS
    uint32_t const dispatch_start = dap_timing_now();
#endif
    __DMB();
    uint8_t const bulk = dap_shared.request_head;
    uint8_t const hid = dap_shared.hid_request_head;
    uint8_t const out = dap_shared.response_tail;
    bool const bulk_ready = !dap_usb_slot_free(dap_shared.request_len, bulk);
    bool const hid_ready = !dap_usb_slot_free(dap_shared.hid_request_len, hid);
    if (!dap_core_has_work(dap_usb_slot_free(dap_shared.response_len, out), bulk_ready, hid_ready)) {
        return false;
    }
    __DMB();

    bool const use_hid =
        dap_usb_choose_hid(hid_ready, dap_shared.hid_request_seq[hid], bulk_ready, dap_shared.request_seq[bulk]);
    uint32_t const seq = use_hid ? dap_shared.hid_request_seq[hid] : dap_shared.request_seq[bulk];
    uint8_t const stage = use_hid ? dap_shared.hid_request_stage[hid] : dap_shared.request_stage[bulk];
    bool const staged = stage != 0U;
    uint16_t const request_length = use_hid ? dap_shared.hid_request_len[hid] : dap_shared.request_len[bulk];
    uint32_t const packet_size = use_hid ? DAP_SHARED_HID_REPORT_SIZE : DAP_PACKET_SIZE;
    uint8_t* request = request_buffer;
    uint8_t* response = response_buffer;
    if (staged) {
        request = use_hid ? dap_stage.hid_request[stage - 1U] : dap_stage.request[stage - 1U];
        response = dap_stage.response[out];
    } else if (use_hid) {
#ifdef DAP_TIMING_DIAGNOSTICS
        uint32_t const copy_start = dap_timing_now();
#endif
        memcpy(request_copy, dap_shared.hid_request[hid], dap_shared.hid_request_len[hid]);
#ifdef DAP_TIMING_DIAGNOSTICS
        if (request_copy[0] != DAP_TIMING_COMMAND) {
            dap_stage.timing.request_copy_cycles += dap_timing_now() - copy_start;
        }
#endif
    } else {
#ifdef DAP_TIMING_DIAGNOSTICS
        uint32_t const copy_start = dap_timing_now();
#endif
        memcpy(request_copy, dap_shared.request[bulk], dap_shared.request_len[bulk]);
#ifdef DAP_TIMING_DIAGNOSTICS
        if (request_copy[0] != DAP_TIMING_COMMAND) {
            dap_stage.timing.request_copy_cycles += dap_timing_now() - copy_start;
        }
#endif
    }
#ifdef DAP_TIMING_DIAGNOSTICS
    bool const timing_command = !use_hid && (request[0] == DAP_TIMING_COMMAND);
    if (!use_hid && !timing_command) {
        dap_stage.timing.wake_to_dispatch_cycles += dap_timing_now() - dap_stage.request_tick[bulk];
    }
#endif

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
#ifdef DAP_TIMING_DIAGNOSTICS
    if (!timing_command) {
        dap_stage.timing.dispatch_cycles += dap_timing_now() - dispatch_start;
    }
    uint32_t const execute_start = dap_timing_now();
#endif
    MARKER_BUSY();
    uint32_t produced = DAP_ExecuteCommandSized(request, request_length, packet_size, response) & 0xFFFFU;
    MARKER_IDLE();
#ifdef DAP_TIMING_DIAGNOSTICS
    if (!timing_command) {
        dap_stage.timing.dap_execute_cycles += dap_timing_now() - execute_start;
    }
#endif
    if (produced > packet_size) {
        produced = packet_size;
    }

    if (use_hid) {
        produced = dap_hid_reply(request, request_length, response, produced);
    }
    if (produced != 0U) {
        if (!staged) {
#ifdef DAP_TIMING_DIAGNOSTICS
            uint32_t const copy_start = timing_command ? 0U : dap_timing_now();
#endif
            memcpy(dap_shared.response[out], response_copy, produced);
#ifdef DAP_TIMING_DIAGNOSTICS
            if (!timing_command) {
                dap_stage.timing.response_copy_cycles += dap_timing_now() - copy_start;
            }
#endif
        }
#ifdef DAP_TIMING_DIAGNOSTICS
        uint32_t const publish_start = timing_command ? 0U : dap_timing_now();
#endif
        dap_shared.response_staged[out] = staged;
        dap_shared.response_seq[out] = seq;
        dap_shared.response_transport[out] = use_hid ? DAP_TRANSPORT_HID : DAP_TRANSPORT_BULK;
        __DMB();
        dap_shared.response_len[out] = (uint16_t)produced;
        dap_shared.response_tail = dap_usb_next_slot(out, REQUEST_SLOTS);
        wake_core0();
#ifdef DAP_TIMING_DIAGNOSTICS
        if (!timing_command) {
            dap_stage.timing.reply_publish_cycles += dap_timing_now() - publish_start;
        }
#endif
    }
    dap_shared.core1.commands++;
    if (reboot_after_reply != REBOOT_NONE) {
        __DMB();
        dap_shared.reboot_mode = reboot_after_reply;
        reboot_after_reply = REBOOT_NONE;
        wake_core0();
    }
    return true;
}

/** Every exception CPU 1 takes is a fault: it runs with interrupts off. Record it where CPU 0 can
 *  report it over USB, then stop. `frame` is the stacked exception frame, from core1_start.S. */
[[noreturn]] __attribute__((used)) void core1_fault(const uint32_t* frame);
[[noreturn]] __attribute__((used)) void core1_fault(const uint32_t* frame) {
    dap_shared.core1.cfsr = SCB->CFSR;
    dap_shared.core1.hfsr = SCB->HFSR;
    dap_shared.core1.bfar = SCB->BFAR;
    dap_shared.core1.lr = frame[5];
    dap_shared.core1.pc = frame[6];
    dap_shared.core1.xpsr = frame[7];
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
 * In SRAM after the bit paths (src/sample_probe.c says why there), so the spin fetches nothing
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
