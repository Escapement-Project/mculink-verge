/** CMSIS-DAP firmware for the MCU-Link.
 *
 * CPU 0 owns USB and publishes received packets in USB SRAM. CPU 1 executes one DAP command at a
 * time and publishes its response back to CPU 0.
 */
#include <string.h>

#include "bsp/board_api.h"
#include "fsl_device_registers.h"
#include "fsl_clock.h"
#include "fsl_reset.h"
#include "tusb.h"

#include "board_pins.h"
#include "core_clock.h"
#include "dap_core.h"
#include "dap_usb_shared.h"
#include "dap_usb_slots.h"
#include "led.h"
#include "marker.h"
#include "reboot.h"
#include "target_sense.h"
#include "dap_core_security.h"
#include "usb_transport.h"
#include "vcom.h"

// DAP_config.h first: DAP.h reads the DAP_SWD and DAP_JTAG settings it defines.
#include "DAP_config.h"

#include "DAP.h"
#include "dap_dispatch.h"

// USB DMA and the generated transfer paths access these buffers as words.
static CFG_TUD_MEM_SECTION CFG_TUD_MEM_ALIGN uint8_t request[REQUEST_SLOTS][DAP_PACKET_SIZE];
static CFG_TUD_MEM_SECTION CFG_TUD_MEM_ALIGN uint8_t hid_request[DAP_PACKET_SIZE];
static CFG_TUD_MEM_SECTION CFG_TUD_MEM_ALIGN uint8_t response[DAP_USB_RESPONSE_SLOT_SIZE(DAP_RESPONSE_SIZE)];
static CFG_TUD_MEM_SECTION CFG_TUD_MEM_ALIGN uint8_t hid_response[DAP_RESPONSE_SIZE];

CFG_TUD_MEM_SECTION CFG_TUD_MEM_ALIGN dap_usb_shared_state_t usb_shared __attribute__((used));

#define request_len (usb_shared.request_len)
#define request_seq (usb_shared.request_seq)
#define request_head (usb_shared.request_head)
#define request_tail (usb_shared.request_tail)
#define rx_armed_slot (usb_shared.rx_armed_slot)
#define next_request_seq (usb_shared.next_request_seq)
#define hid_request_len (usb_shared.hid_request_len)
#define hid_request_seq (usb_shared.hid_request_seq)
#define response_len (usb_shared.response_len)
#define response_transport (usb_shared.response_transport)
#define response_state (usb_shared.response_state)

uint8_t core1_stack[4096] __attribute__((section("m_usb_global"), aligned(8), used));

enum {
    DAP_TRANSPORT_BULK,
    DAP_TRANSPORT_HID,
};

static bool disconnect_requested;
static bool disconnect_complete;

#ifdef MARKER_PINS
/** Which J8 pins the marker still owns. ID_VENDOR_J8 takes pins away from it and gives them back. */
volatile uint32_t marker_mask = J8_PIN_MASK;
#endif

static void hid_report_packet_size(const uint8_t* req, uint8_t* resp);
static bool dap_task(void);
static void usb_transport_task(void);
static void start_core1(void);

static void signal_core1(void) {
    MAILBOX->MBOXIRQ[0].IRQSET = DAP_CORE_MAILBOX_WAKE_MASK;
    __DSB();
}

/** One received packet, which is one command. This callback runs on CPU 0. */
void tud_vendor_rx_cb(uint8_t index, const uint8_t* buffer, uint32_t length) {
    (void)index;
    if ((length == 0U) || (length > DAP_PACKET_SIZE)) {
        return;
    }

    // The abort must reach an active transfer before the command queue can delay it.
    if (buffer[0] == ID_DAP_TransferAbort) {
        DAP_TransferAbort = 1U;
        __DMB();
        signal_core1();
        return;
    }
    if (buffer != request[rx_armed_slot]) {
        return;
    }

    request_seq[request_tail] = next_request_seq++;
    __DMB();
    request_len[request_tail] = (uint16_t)length;
    __DMB();
    request_tail = dap_usb_next_slot(request_tail, REQUEST_SLOTS);
    signal_core1();
}

void tud_vendor_tx_cb(uint8_t index, uint32_t sent_bytes) {
    (void)index;
    (void)sent_bytes;
    if (response_transport == DAP_TRANSPORT_BULK) {
        dap_usb_response_complete(&response_state);
        __DMB();
        signal_core1();
    }
}

static bool start_rx(void* context, uint8_t slot) {
    uint8_t (*buffers)[DAP_PACKET_SIZE] = context;
    return tud_vendor_read_xfer_buf(buffers[slot]);
}

static bool start_tx(void* context, uint8_t slot, uint16_t length) {
    (void)slot;
    return tud_vendor_write_xfer(context, length) == length;
}

/** Keep TinyUSB and endpoint ownership on CPU 0. */
static void usb_transport_task(void) {
    bool const vendor_mounted = tud_vendor_mounted();
    if (vendor_mounted) {
        (void)dap_usb_arm_rx(dap_usb_request_slot_free(request_len, request_tail), request_tail, &rx_armed_slot,
                             start_rx, request);
    }

    if (disconnect_requested) {
        tud_disconnect();
        disconnect_complete = true;
        disconnect_requested = false;
    }

    if (response_state != DAP_USB_RESPONSE_READY) {
        return;
    }

    __DMB();
    if (!dap_usb_response_start(&response_state)) {
        return;
    }

    bool started;
    if (response_transport == DAP_TRANSPORT_BULK) {
        started = start_tx(response, 0U, response_len);
    } else {
        started = tud_hid_n_report(0U, 0U, hid_response, CFG_TUD_HID_EP_BUFSIZE);
    }
    if (!started) {
        dap_usb_response_retry(&response_state);
    }
}

bool usb_transport_response_idle(void) {
    __DMB();
    return response_state == DAP_USB_RESPONSE_FREE;
}

void usb_transport_request_disconnect(void) {
    disconnect_complete = false;
    disconnect_requested = true;
}

bool usb_transport_disconnect_complete(void) { return disconnect_complete; }

static bool dap_task(void) {
    __DMB();
    bool const bulk_ready = request_len[request_head] != 0U;
    bool const hid_ready = hid_request_len != 0U;
    if (!dap_core_has_work(response_state == DAP_USB_RESPONSE_FREE, bulk_ready, hid_ready)) {
        return false;
    }
    __DMB();

    bool const use_hid = dap_usb_choose_hid(hid_ready, hid_request_seq, bulk_ready, request_seq[request_head]);
    const uint8_t* request_data;
    if (use_hid) {
        request_data = hid_request;
        response_transport = DAP_TRANSPORT_HID;
    } else {
        request_data = request[request_head];
        response_transport = DAP_TRANSPORT_BULK;
    }

    led_activity();
    MARKER_BUSY();
    uint32_t produced = DAP_ExecuteCommand(request_data, use_hid ? hid_response : response) & 0xFFFFU;
    MARKER_IDLE();
    if (produced > DAP_PACKET_SIZE) {
        produced = DAP_PACKET_SIZE;
    }

    if (use_hid) {
        hid_request_len = 0U;
        __DMB();
        response_len = CFG_TUD_HID_EP_BUFSIZE;
        hid_report_packet_size(request_data, hid_response);
    } else {
        __DMB();
        request_head = dap_usb_request_release(request_len, request_head, REQUEST_SLOTS);
        __DMB();
        response_len = (uint16_t)produced;
    }

    if (response_len != 0U) {
        __DMB();
        response_state = DAP_USB_RESPONSE_READY;
    }
    return true;
}

/** Answer the v1 host with the v1 packet size. */
static void hid_report_packet_size(const uint8_t* req, uint8_t* resp) {
    if ((req[0] == ID_DAP_Info) && (req[1] == DAP_ID_PACKET_SIZE)) {
        resp[1] = 2U;
        resp[2] = (uint8_t)(CFG_TUD_HID_EP_BUFSIZE & 0xFFU);
        resp[3] = (uint8_t)(CFG_TUD_HID_EP_BUFSIZE >> 8);
    }
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t* buffer,
                               uint16_t reqlen) {
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const* buffer,
                           uint16_t bufsize) {
    (void)instance;
    (void)report_id;
    (void)report_type;

    if ((bufsize == 0U) || (bufsize > DAP_PACKET_SIZE) || (hid_request_len != 0U)) {
        return;
    }

    memcpy(hid_request, buffer, bufsize);
    hid_request_seq = next_request_seq++;
    __DMB();
    hid_request_len = bufsize;
    __DMB();
    signal_core1();
}

void tud_hid_report_complete_cb(uint8_t instance, uint8_t const* report, uint16_t len) {
    (void)instance;
    (void)report;
    (void)len;
    if (response_transport == DAP_TRANSPORT_HID) {
        dap_usb_response_complete(&response_state);
        __DMB();
        signal_core1();
    }
}

void tud_hid_report_failed_cb(uint8_t instance, hid_report_type_t report_type,
                              uint8_t const* report, uint16_t xferred_bytes) {
    (void)instance;
    (void)report_type;
    (void)report;
    (void)xferred_bytes;
    if (response_transport == DAP_TRANSPORT_HID) {
        dap_usb_response_retry(&response_state);
    }
}

/** Initialize the DAP core, then sleep until CPU 0 publishes another request. */
__attribute__((section(".core1_code.text"), noinline, noreturn)) void dap_core1_main(void) {
    __disable_irq();
    dap_core1_mailbox_wait_init();

    for (;;) {
        if (dap_task()) {
            continue;
        }

        volatile MAILBOX_Type* const mailbox = (volatile MAILBOX_Type*)0x4008B000U;
        uint32_t const pending = dap_core_mailbox_ack_mask(mailbox->MBOXIRQ[0].IRQ);
        if (pending != 0U) {
            mailbox->MBOXIRQ[0].IRQCLR = pending;
        }
        NVIC_ClearPendingIRQ(MAILBOX_IRQn);
        __DSB();
        __DMB();
        if (!dap_task()) {
            __WFE();
        }
    }
}

/** Use a pending mailbox IRQ as a WFE event without running an ISR during DAP transfers. */
__attribute__((section(".core1_code.text"), noinline)) void dap_core1_mailbox_wait_init(void) {
    SCB->SCR |= SCB_SCR_SEVONPEND_Msk;
    NVIC_ClearPendingIRQ(MAILBOX_IRQn);
    NVIC_EnableIRQ(MAILBOX_IRQn);
    __DSB();
    __ISB();
}

static void start_core1(void) {
    extern uint32_t core1_vectors[];
    (void)core1_vectors;

    SYSCON->CPUCFG |= SYSCON_CPUCFG_CPU1ENABLE_MASK;
    SYSCON->CPBOOT = SYSCON_CPBOOT_CPBOOT((uint32_t)core1_vectors);
    uint32_t const control = SYSCON->CPUCTRL | 0xC0C48000U;
    SYSCON->CPUCTRL = control | SYSCON_CPUCTRL_CPU1RSTEN_MASK | SYSCON_CPUCTRL_CPU1CLKEN_MASK;
    SYSCON->CPUCTRL = (control | SYSCON_CPUCTRL_CPU1CLKEN_MASK) & ~SYSCON_CPUCTRL_CPU1RSTEN_MASK;
    __DSB();
    __ISB();
}

__attribute__((noinline, used)) void dap_core_security_init(void) {
    dap_core_security_apply((volatile uint32_t*)0x500AC000U);
    __DSB();
    __ISB();
}

int main(void) {
    board_init();
    reboot_clear_boot_mode();
    board_core_clock_init();
    board_clocks_init();
    target_sense_init();
    led_init();
    vcom_init();
    board_timestamp_init();
    DAP_Setup();
    CLOCK_EnableClock(kCLOCK_Mailbox);
    RESET_PeripheralReset(kMAILBOX_RST_SHIFT_RSTn);
    MAILBOX->MBOXIRQ[0].IRQCLR = UINT32_MAX;
    dap_usb_shared_init(&usb_shared);
    DAP_TransferAbort = 0U;
    dap_core_security_init();
    start_core1();

    board_init_after_tusb();
    tusb_rhport_init_t const init = {
        .role = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_AUTO,
    };
    tusb_init(BOARD_TUD_RHPORT, &init);
    __DMB();
    __enable_irq();

    for (;;) {
        tud_task();
        usb_transport_task();
        vcom_task();
        reboot_poll();
    }
}
