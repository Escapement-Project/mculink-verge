/** CMSIS-DAP firmware for the MCU-Link: CPU 0's image.
 *
 * CPU 0 owns USB and publishes received packets in USB SRAM. CPU 1 runs its own image, embedded in
 * this one by core1_image.S, executes one DAP command at a time and publishes its response back to
 * CPU 0. Nothing here waits on CPU 1, so a CPU 1 that has stopped answering can still be sent to
 * the bootloader over USB.
 */
#include <string.h>

#include "bsp/board_api.h"
#include "fsl_device_registers.h"
#include "fsl_clock.h"
#include "fsl_iocon.h"
#include "fsl_reset.h"
#include "tusb.h"

#include "board_pins.h"
#include "board_straps.h"
#include "core_clock.h"
#include "dap_core.h"
#include "dap_core_security.h"
#include "dap_shared.h"
#ifdef DAP_TIMING_DIAGNOSTICS
#include "dap_timing.h"
#endif
#include "energy_monitor.h"
#include "dap_usb_slots.h"
#include "led.h"
#include "reboot.h"
#include "usb_descriptors.h"
#include "usb_transport.h"
#include "vcom.h"

// DAP_config.h first: DAP.h reads the DAP_SWD and DAP_JTAG settings it defines.
#include "DAP_config.h"

#include "DAP.h"

static_assert(DAP_PACKET_SIZE == DAP_SHARED_PACKET_SIZE, "dap_shared.h repeats DAP_PACKET_SIZE");
static_assert(CFG_TUD_HID_EP_BUFSIZE == DAP_SHARED_HID_REPORT_SIZE, "dap_shared.h repeats the HID report size");

dap_shared_t dap_shared __attribute__((section(".dap_shared")));
dap_stage_t dap_stage __attribute__((section(".dap_stage")));

/** The dap_stage request buffer each transport's next staged command goes in. */
static uint8_t stage_next;
static uint8_t hid_stage_next;

/** Copy a command into the next stage buffer, and return what its slot's `_stage` takes. */
__attribute__((section(".ramfunc"))) static uint8_t stage_request(uint8_t (*buffers)[DAP_SHARED_PACKET_SIZE],
                                                                 uint8_t* next, const uint8_t* command,
                                                                 uint32_t length) {
    if (dap_shared.stage_commands == 0U) {
        return 0U;
    }
    uint8_t const at = *next;
    *next = dap_usb_next_slot(at, DAP_STAGE_REQUESTS);
#ifdef DAP_TIMING_DIAGNOSTICS
    bool const record_timing = command[0] != DAP_TIMING_COMMAND;
    uint32_t const copy_start = record_timing ? dap_timing_now() : 0U;
#endif
    memcpy(buffers[at], command, length);
#ifdef DAP_TIMING_DIAGNOSTICS
    if (record_timing) {
        dap_stage.timing.rx_stage_copy_cycles += dap_timing_now() - copy_start;
    }
#endif
    return (uint8_t)(at + 1U);
}

static bool disconnect_requested;
static bool disconnect_complete;

/** The reply at response_head is on its way to the host. At most one is: both transports send
 *  from the head of the one ring, so replies leave in the order their commands ran. */
static volatile bool response_in_flight;

static void usb_transport_task(void);
static void energy_range_retry_task(void);
static void energy_control_task(void);
static void arm_rx(void);
static void send_head(void);
static void start_core1(void);

__attribute__((section(".ramfunc"))) static void signal_core1(void) {
    MAILBOX->MBOXIRQ[DAP_CORE_MAILBOX_CPU1].IRQSET = DAP_CORE_MAILBOX_WAKE_MASK;
    __DSB();
}

/** DAP_TransferAbort skips the queue on either transport: it has to reach the transfer running now,
 *  not wait behind it. Before CPU 1 has published the flag there is no transfer to abort. A
 *  faulting CPU 1 can leave anything in the pointer, and a stray write would take CPU 0 and the
 *  way back to the bootloader with it. */
__attribute__((section(".ramfunc"))) static void transfer_abort(void) {
    uintptr_t const abort = (uintptr_t)dap_shared.transfer_abort;
    if ((abort >= CORE1_RAM_ORIGIN) && (abort < CORE1_RAM_ORIGIN + CORE1_RAM_LENGTH)) {
        *(volatile uint8_t*)abort = 1U;
    }
    __DMB();
    signal_core1();
}

/** One received packet, which is one command. This callback runs on CPU 0. */
__attribute__((section(".ramfunc"))) void tud_vendor_rx_cb(uint8_t index, const uint8_t* buffer, uint32_t length) {
    (void)index;
    if ((length == 0U) || (length > DAP_PACKET_SIZE)) {
        return;
    }
    if (buffer[0] == ID_DAP_TransferAbort) {
        transfer_abort();
        return;
    }
    if (buffer != dap_shared.request[dap_shared.rx_armed_slot]) {
        return;
    }

#ifdef DAP_TIMING_DIAGNOSTICS
    bool const record_timing = buffer[0] != DAP_TIMING_COMMAND;
    uint32_t const callback_start = record_timing ? dap_timing_now() : 0U;
#endif
    uint8_t const tail = dap_shared.request_tail;
    dap_shared.request_stage[tail] = stage_request(dap_stage.request, &stage_next, buffer, length);
#ifdef DAP_TIMING_DIAGNOSTICS
    uint32_t const publish_start = record_timing ? dap_timing_now() : 0U;
#endif
    dap_shared.request_seq[tail] = dap_shared.next_request_seq++;
    __DMB();
    dap_shared.request_len[tail] = (uint16_t)length;
    __DMB();
#ifdef DAP_TIMING_DIAGNOSTICS
    dap_stage.request_tick[tail] = dap_timing_now();
#endif
    dap_shared.request_tail = dap_usb_next_slot(dap_shared.request_tail, REQUEST_SLOTS);
#ifdef DAP_TIMING_DIAGNOSTICS
    if (record_timing) {
        dap_stage.timing.rx_publish_cycles += dap_timing_now() - publish_start;
    }
    uint32_t const signal_start = record_timing ? dap_timing_now() : 0U;
#endif
    signal_core1();
#ifdef DAP_TIMING_DIAGNOSTICS
    if (record_timing) {
        dap_stage.timing.mailbox1_signal_cycles += dap_timing_now() - signal_start;
        dap_stage.timing.rx_count++;
    }
#endif
    arm_rx();
#ifdef DAP_TIMING_DIAGNOSTICS
    if (record_timing) {
        dap_stage.timing.rx_callback_cycles += dap_timing_now() - callback_start;
    }
#endif
}

/** TinyUSB reports no bus reset to the application. This runs at each configuration the host sets,
 *  and one follows every reset. The reset ended every transfer TinyUSB had started without a
 *  completion, so the reply in flight would never leave, and what the last host left queued would
 *  answer the next one's commands. Each configuration starts a new numbering instead: CPU 1 drops
 *  older commands unrun, CPU 0 drops their replies unsent, and the abort cuts short the command
 *  CPU 1 may be running. A command CPU 1 had already found live when the abort landed runs to its
 *  end, because starting a transfer clears the flag. */
void tud_mount_cb(void) {
    dap_shared.first_live_seq = dap_shared.next_request_seq;
    response_in_flight = false;
    __DMB();
    transfer_abort();
}

/** The head reply has left, or will not: its slot goes back to CPU 1, which may be waiting for one. */
__attribute__((section(".ramfunc"))) static void response_sent(void) {
#ifdef DAP_TIMING_DIAGNOSTICS
    uint8_t const head = dap_shared.response_head;
    if (dap_stage.response_timing_excluded[head] == 0U) {
        dap_stage.timing.tx_in_flight_cycles += dap_timing_now() - dap_stage.response_tx_tick[head];
        dap_stage.timing.tx_count++;
    }
    dap_stage.response_timing_excluded[head] = 0U;
#endif
    dap_shared.response_head = dap_usb_slot_release(dap_shared.response_len, dap_shared.response_head, REQUEST_SLOTS);
    response_in_flight = false;
    __DMB();
    signal_core1();
}

__attribute__((section(".ramfunc"))) static bool head_in_flight_on(uint8_t transport) {
    return response_in_flight && (dap_shared.response_transport[dap_shared.response_head] == transport);
}

__attribute__((section(".ramfunc"))) void tud_vendor_tx_cb(uint8_t index, uint32_t sent_bytes) {
    (void)index;
    (void)sent_bytes;
    if (head_in_flight_on(DAP_TRANSPORT_BULK)) {
        response_sent();
        send_head();
    }
}

__attribute__((section(".ramfunc"))) static bool start_rx(void* context, uint8_t slot) {
    uint8_t (*buffers)[DAP_PACKET_SIZE] = context;
    return tud_vendor_read_xfer_buf(buffers[slot]);
}

__attribute__((section(".ramfunc"))) static bool start_tx(void* context, uint8_t slot, uint16_t length) {
    uint8_t (*buffers)[DAP_PACKET_SIZE] = context;
#ifdef DAP_TIMING_DIAGNOSTICS
    bool const staged = dap_shared.response_staged[slot] != 0U;
    bool const record_timing = staged ? dap_stage.response[slot][0] != DAP_TIMING_COMMAND
                                      : buffers[slot][0] != DAP_TIMING_COMMAND;
    uint32_t const copy_start = record_timing ? dap_timing_now() : 0U;
#endif
    if (dap_shared.response_staged[slot] != 0U) {
        if (length == 7U) {
            memcpy(buffers[slot], dap_stage.response[slot], 4U);
            memcpy(buffers[slot] + 4U, dap_stage.response[slot] + 4U, 3U);
        } else {
            memcpy(buffers[slot], dap_stage.response[slot], length);
        }
#ifdef DAP_TIMING_DIAGNOSTICS
        if (record_timing) {
            dap_stage.timing.tx_stage_copy_cycles += dap_timing_now() - copy_start;
        }
#endif
    }
    bool started;
#ifdef DAP_TIMING_DIAGNOSTICS
    dap_stage.response_timing_excluded[slot] = !record_timing;
    uint32_t const start = record_timing ? dap_timing_now() : 0U;
    if (record_timing) {
        dap_stage.response_tx_tick[slot] = dap_timing_now();
    }
#endif
    if (dap_shared.response_transport[slot] == DAP_TRANSPORT_BULK) {
        started = tud_vendor_write_xfer(buffers[slot], length) == length;
    } else {
        started = tud_hid_n_report(0U, 0U, buffers[slot], length);
    }
#ifdef DAP_TIMING_DIAGNOSTICS
    if (record_timing) {
        dap_stage.timing.tx_start_cycles += dap_timing_now() - start;
    }
#endif
    return started;
}

/** Keep TinyUSB and endpoint ownership on CPU 0. */
__attribute__((section(".ramfunc"))) static void usb_transport_task(void) {
    arm_rx();
    if (disconnect_requested) {
        tud_disconnect();
        disconnect_complete = true;
        disconnect_requested = false;
    }
    send_head();
}

/** Retry the Pro's range latch outside the DMA interrupt. Four segment intervals limit repeated
 *  reset pulses while leaving time for a range change to settle. */
__attribute__((section(".ramfunc"))) static void energy_range_retry_task(void) {
    static uint32_t last_retry_segment;
    if ((dap_shared.energy_control_ack & DAP_ENERGY_QUIET_FLAG) != 0U ||
        dap_shared.energy_range_retry_pending == 0U) {
        return;
    }
    dap_shared.energy_range_retry_active = 1U;
    __DMB();
    if ((dap_shared.energy_range_retry_enabled == 0U) ||
        (dap_shared.energy_range_retry_pending == 0U)) {
        dap_shared.energy_range_retry_active = 0U;
        return;
    }
    dap_shared.energy_range_retry_pending = 0U;
    uint32_t const segments = dap_shared.energy_segments;
    if ((segments - last_retry_segment) < 4U) {
        dap_shared.energy_range_retry_active = 0U;
        return;
    }
    uint32_t const range_pins = GPIO->PIN[0];
    if (((range_pins & (1UL << 29U)) == 0U) || ((range_pins & (1UL << 30U)) != 0U)) {
        dap_shared.energy_range_retry_active = 0U;
        return;
    }
    last_retry_segment = segments;
    uint32_t const mux = IOCON->PIO[0][29];
    uint32_t const p29 = 1UL << 29U;
    uint32_t const p17 = 1UL << 17U;
    GPIO->SET[0] = p29;
    GPIO->DIRSET[0] = p29;
    IOCON->PIO[0][29] = IOCON_FUNC0 | IOCON_MODE_INACT | IOCON_DIGITAL_EN;
    __DSB();
    for (volatile uint32_t reset_release = 0U; reset_release < 30000U; reset_release++) {
        __NOP();
    }
    GPIO->CLR[0] = p17;
    __DSB();
    for (volatile uint32_t pulse_hold = 0U; pulse_hold < 3000U; pulse_hold++) {
        __NOP();
    }
    GPIO->SET[0] = p17;
    __DSB();
    for (volatile uint32_t pulse_hold = 0U; pulse_hold < 3000U; pulse_hold++) {
        __NOP();
    }
    IOCON->PIO[0][29] = mux;
    GPIO->DIRCLR[0] = p29;
    __DSB();
    dap_shared.energy_range_retry_active = 0U;
}

/** Main-loop acknowledgement follows the previous handler's segment-count publication. */
__attribute__((section(".ramfunc"), noinline)) static void energy_control_task(void) {
    uint32_t const requested = dap_shared.energy_control_request;
    if (requested != dap_shared.energy_control_ack) {
        uint32_t const primask = __get_PRIMASK();
        __disable_irq();
        __DMB();
        dap_shared.energy_control_ack = requested;
        __DMB();
        __set_PRIMASK(primask);
    }
}

/** Arms the OUT endpoint into the tail request slot if that is free. Arming claims the endpoint,
 *  so a call while it is armed changes nothing. The receive callback calls it as well, so the
 *  next request can arrive without the loop coming round. */
__attribute__((section(".ramfunc"))) static void arm_rx(void) {
    if (tud_vendor_mounted()) {
        (void)dap_usb_arm_rx(dap_usb_slot_free(dap_shared.request_len, dap_shared.request_tail),
                             dap_shared.request_tail, &dap_shared.rx_armed_slot, start_rx, dap_shared.request);
    }
}

/** Starts the head reply if CPU 1 has published it and none is on its way. The completion
 *  callbacks call it as well, so a reply already waiting leaves without the loop coming round. */
__attribute__((section(".ramfunc"))) static void send_head(void) {
    uint8_t const head = dap_shared.response_head;
    uint16_t const length = dap_shared.response_len[head];
    if (length == 0U) {
        return;
    }
    __DMB();
    if (dap_usb_stale(dap_shared.response_seq[head], dap_shared.first_live_seq)) {
        response_sent();
        return;
    }
    (void)dap_usb_start_tx(&response_in_flight, head, length, start_tx, dap_shared.response);
}

/** CPU 0 waits here between events, running from SRAM with interrupts masked, so that one pending
 *  shows in ISPR instead of being taken.
 *
 * Measured while CPU 1's per-command code ran from flash: CPU 0 fetching its own loop from flash
 * slowed it by 20 us in a 126-word block read, 270 us of execution against 250. CPU 1 still runs
 * every command it does not route from flash. CPU 0 reading USB SRAM cost nothing. Sleeping in WFE
 * instead kept the same gain but added 5 us to waking for each request.
 */
__attribute__((section(".ramfunc"), noinline)) static void wait_for_work(void) {
    for (;;) {
        if ((MAILBOX->MBOXIRQ[DAP_CORE_MAILBOX_CPU0].IRQ != 0U) || vcom_ready()) {
            return;
        }
        for (uint32_t i = 0U; i < (NUMBER_OF_INT_VECTORS - 16U + 31U) / 32U; i++) {
            if ((NVIC->ISPR[i] & NVIC->ISER[i]) != 0U) {
                return;
            }
        }
    }
}

#ifdef CODE_LAB
/** While a code lab body holds this bit of CPU 0's mailbox, CPU 0 copies 256 bytes back and forth
 *  at the 256-byte-aligned address in the register's upper bits, word by word or with memcpy if
 *  bit 2 is set too, so that the body can time its own accesses against another master on the
 *  same SRAM port. It runs in place of idle()'s wait, and stops for a USB event or CPU 1's wake. */
#define LAB_HAMMER_MASK 2U
#define LAB_HAMMER_MEMCPY 4U

__attribute__((section(".ramfunc"))) static bool lab_hammer(void) {
    uint32_t control = MAILBOX->MBOXIRQ[DAP_CORE_MAILBOX_CPU0].IRQ;
    if ((control & LAB_HAMMER_MASK) == 0U) {
        return false;
    }
    while (((control & 0xFBU) == LAB_HAMMER_MASK) && !tud_task_event_ready()) {
        uint32_t* const words = (uint32_t*)(control & ~0xFFU);
        if ((control & LAB_HAMMER_MEMCPY) != 0U) {
            memcpy(&words[64], words, 256U);
        } else {
            for (uint32_t i = 0U; i < 64U; i++) {
                ((volatile uint32_t*)words)[64U + i] = ((volatile uint32_t*)words)[i];
            }
        }
        control = MAILBOX->MBOXIRQ[DAP_CORE_MAILBOX_CPU0].IRQ;
    }
    return true;
}
#endif

/** Everything that gives the loop work wakes it: USB and the LED tick by interrupt, CPU 1 through
 *  the mailbox when it frees a request slot, publishes a reply or asks for a reboot, and the
 *  console through vcom_ready. The mailbox is cleared before the checks, so a write CPU 1 makes
 *  after them still ends the wait. */
__attribute__((section(".ramfunc"))) static void idle(void) {
    MAILBOX->MBOXIRQ[DAP_CORE_MAILBOX_CPU0].IRQCLR = DAP_CORE_MAILBOX_WAKE_MASK;
    __DSB();
    bool const reply_waiting =
        !response_in_flight && !dap_usb_slot_free(dap_shared.response_len, dap_shared.response_head);
    if (reply_waiting || disconnect_requested || tud_task_event_ready() ||
        dap_shared.energy_control_request != dap_shared.energy_control_ack) {
        return;
    }
#ifdef CODE_LAB
    if (lab_hammer()) {
        return;
    }
#endif
    __disable_irq();
    wait_for_work();
    __enable_irq();
}

/** The startup file's vector table names it, weakly; this is the strong definition. */
void DMA0_IRQHandler(void);

/** DMA0 has finished a VCOM transfer or one segment of SWO or energy capture. CPU 0 counts both
 *  rings; each CPU 1 reader turns the segment count into a position. Completions are cleared before
 *  the counts move, so a reader can find a ring one segment short but never long. */
__attribute__((section(".ramfunc"))) void DMA0_IRQHandler(void) {
    // An error raises this interrupt too, and until it is cleared the interrupt comes straight
    // back, ahead of everything else on this core. SWO's is CPU 1's to report, so hand it over.
    uint32_t const energy_mask = (dap_shared.energy_control_ack & DAP_ENERGY_QUIET_FLAG) != 0U ? 0U : ENERGY_DMA_MASK;
    uint32_t const failed = DMA0->COMMON[0].ERRINT & (SWO_DMA_MASK | VCOM_DMA_MASK | energy_mask);
    if (failed != 0U) {
        DMA0->COMMON[0].ERRINT = failed;
        if ((failed & SWO_DMA_MASK) != 0U) {
            dap_shared.swo_dma_error = 1U;
        }
        if ((failed & ENERGY_DMA_MASK) != 0U) {
            dap_shared.energy_dma_error = 1U;
        }
        vcom_dma_error(failed);
    }
    uint32_t const done = DMA0->COMMON[0].INTA & (SWO_DMA_MASK | VCOM_DMA_MASK | energy_mask);
    if (done == 0U) {
        return;
    }
    if ((done & ENERGY_DMA_MASK) != 0U) {
        dap_shared.energy_dma_sequence++;
        __DMB();
    }
    DMA0->COMMON[0].INTA = done;
    __DMB();
    if ((done & SWO_DMA_MASK) != 0U) {
        dap_shared.swo_segments++;
    }
    if ((done & ENERGY_DMA_MASK) != 0U) {
        if (dap_shared.energy_segment_handler != nullptr) {
            dap_shared.energy_segment_handler();
        }
        uint32_t const range_pins = GPIO->PIN[0];
        if (((range_pins & (1UL << 29U)) != 0U) && ((range_pins & (1UL << 30U)) == 0U)) {
            dap_shared.energy_range_retry_pending = 1U;
        }
        dap_shared.energy_segments++;
        __DMB();
        dap_shared.energy_dma_sequence++;
    }
    vcom_dma_done(done);
}

/** DMA0 is CPU 0's: the consoles use channels 4 to 7, and CPU 1 uses channels 8 and 21 for SWO and
 *  energy capture. Only CPU 0 resets the controller or points it at its table. Its table is in USB
 *  SRAM, so set its security rules before DMA fetches a descriptor. */
__attribute__((noinline)) void dma0_init(void);
__attribute__((noinline)) void dma0_init(void) {
    CLOCK_EnableClock(kCLOCK_Dma0);
    RESET_PeripheralReset(kDMA0_RST_SHIFT_RSTn);
    DMA0->SRAMBASE = (uint32_t)dap_shared.dma_table;
    DMA0->CTRL = DMA_CTRL_ENABLE_MASK;
}

bool usb_transport_reply_in_flight(void) { return response_in_flight; }

bool usb_transport_response_idle(void) {
    __DMB();
    return !response_in_flight && dap_usb_slot_free(dap_shared.response_len, dap_shared.response_head);
}

void usb_transport_request_disconnect(void) {
    disconnect_complete = false;
    disconnect_requested = true;
}

bool usb_transport_disconnect_complete(void) { return disconnect_complete; }

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t* buffer,
                               uint16_t reqlen) {
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0;
}

__attribute__((section(".ramfunc"))) void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                                                                hid_report_type_t report_type, uint8_t const* buffer,
                                                                uint16_t bufsize) {
    (void)instance;
    (void)report_id;
    (void)report_type;

    if ((bufsize == 0U) || (bufsize > DAP_SHARED_HID_REPORT_SIZE)) {
        return;
    }
    if (buffer[0] == ID_DAP_TransferAbort) {
        transfer_abort();
        return;
    }
    // Full only for a host with more reports outstanding than the packet count it was told.
    // TinyUSB re-arms the endpoint when this returns, so there is nowhere to hold the report.
    uint8_t const tail = dap_shared.hid_request_tail;
    if (!dap_usb_slot_free(dap_shared.hid_request_len, tail)) {
        return;
    }

    uint8_t const stage = stage_request(dap_stage.hid_request, &hid_stage_next, buffer, bufsize);
    if (stage == 0U) {
        memcpy(dap_shared.hid_request[tail], buffer, bufsize);
    }
    dap_shared.hid_request_stage[tail] = stage;
    dap_shared.hid_request_seq[tail] = dap_shared.next_request_seq++;
    __DMB();
    dap_shared.hid_request_len[tail] = bufsize;
    __DMB();
    dap_shared.hid_request_tail = dap_usb_next_slot(tail, REQUEST_SLOTS);
    signal_core1();
}

__attribute__((section(".ramfunc"))) void tud_hid_report_complete_cb(uint8_t instance, uint8_t const* report,
                                                                     uint16_t len) {
    (void)instance;
    (void)report;
    (void)len;
    if (head_in_flight_on(DAP_TRANSPORT_HID)) {
        response_sent();
        send_head();
    }
}

/** TinyUSB reports a failed OUT report here too, and that is no reply of ours. */
__attribute__((section(".ramfunc"))) void tud_hid_report_failed_cb(uint8_t instance, hid_report_type_t report_type,
                                                                   uint8_t const* report, uint16_t xferred_bytes) {
    (void)instance;
    (void)report;
    (void)xferred_bytes;
    if ((report_type == HID_REPORT_TYPE_INPUT) && head_in_flight_on(DAP_TRANSPORT_HID)) {
        response_in_flight = false;  // usb_transport_task sends it again
    }
}

static void start_core1(void) {
    extern uint32_t core1_image[];

    // CPU 1's data bus wins every tie at an AHB matrix port. At the reset priorities CPU 0 does,
    // and its copies in dap_stage delayed CPU 1's accesses there by a cycle at a time: enough to
    // move a wire edge when a transfer reads or writes its buffer between two of them.
    SYSCON->AHBMATPRIO = SYSCON_AHBMATPRIO_PRI_CPU1_SBUS(3U);

    SYSCON->CPUCFG |= SYSCON_CPUCFG_CPU1ENABLE_MASK;
    SYSCON->CPBOOT = SYSCON_CPBOOT_CPBOOT((uint32_t)core1_image);
    uint32_t const control = SYSCON->CPUCTRL | 0xC0C48000U;
    SYSCON->CPUCTRL = control | SYSCON_CPUCTRL_CPU1RSTEN_MASK | SYSCON_CPUCTRL_CPU1CLKEN_MASK;
    SYSCON->CPUCTRL = (control | SYSCON_CPUCTRL_CPU1CLKEN_MASK) & ~SYSCON_CPUCTRL_CPU1RSTEN_MASK;
    __DSB();
    __ISB();
}

__attribute__((noinline, used)) void dap_core_security_init(void) {
    // The controller denies non-secure accesses, and the manual addresses it at its secure alias,
    // bit 28 set (UM11126 Table 873). The SDK names that alias only in builds with -mcmse.
    dap_core_security_apply((volatile uint32_t*)(AHB_SECURE_CTRL_BASE | 0x10000000U));
    __DSB();
    __ISB();
}

/** CPU 0's loop. It and everything a command passes through on CPU 0 run from SRAM: from flash
 *  they were the limit on short commands. tools/image.py checks that none of it branches back
 *  into flash. */
[[noreturn]] __attribute__((section(".ramfunc"), noinline)) static void serve(void) {
    for (;;) {
        energy_control_task();
        energy_range_retry_task();
        tud_task();
        usb_transport_task();
        if (vcom_ready()) {
            vcom_task();
        }
        reboot_poll();
        idle();
    }
}

int main(void) {
    board_init();
    reboot_clear_boot_mode();
    board_core_clock_init();
    board_clocks_init();
    dap_shared_init(&dap_shared);
#ifdef DAP_TIMING_DIAGNOSTICS
    dap_timing_init();
#endif
    board_read_straps();
    led_init();
    CLOCK_EnableClock(kCLOCK_Mailbox);
    RESET_PeripheralReset(kMAILBOX_RST_SHIFT_RSTn);
    MAILBOX->MBOXIRQ[0].IRQCLR = UINT32_MAX;
    dap_core_security_init();
    dma0_init();
    vcom_init();
    // The highest priority, so nothing comes between DMA0_IRQHandler reading a completion and
    // counting it, which src/swo.c's capture start waits out.
    NVIC_SetPriority(DMA0_IRQn, 0U);
    NVIC_EnableIRQ(DMA0_IRQn);
    start_core1();

    board_init_after_tusb();
    usb_descriptors_init();
    tusb_rhport_init_t const init = {
        .role = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_AUTO,
    };
    tusb_init(BOARD_TUD_RHPORT, &init);
    __DMB();
    __enable_irq();

    serve();
}
