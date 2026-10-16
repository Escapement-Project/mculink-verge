/** CMSIS-DAP firmware for the MCU-Link.
 *
 * One DAP command per USB packet, which is what CMSIS-DAP v2 specifies: the host sends a request
 * and waits for exactly one response before sending the next. There is no queue here because the
 * protocol does not need one -- DAP_ExecuteCommands batches on the host's behalf inside a single
 * packet instead.
 */
#include <string.h>

#include "bsp/board_api.h"
#include "tusb.h"

#include "board_pins.h"
#include "led.h"
#include "marker.h"
#include "reboot.h"
#include "target_sense.h"
#include "core_clock.h"
#include "vcom.h"

// DAP_config.h first: DAP.h reads the DAP_SWD and DAP_JTAG settings it defines, so a sorted
// include block that puts DAP.h ahead of it will not compile.
#include "DAP_config.h"

#include "DAP.h"
#include "dap_dispatch.h"

/** Two request slots, so the host can have a second command in flight while the first runs.
 *
 * With one, the wire sits idle from the moment a reply goes out until the host has noticed it and
 * sent the next request. Measured on a 64-word block read, a second command in flight is worth
 * 11%, and a third no more than that.
 *
 * **Three slots, for two in flight.** A host keeps its pipeline one packet short of what the
 * probe reports, because the spare buffer is where DAP_TransferAbort has to land to overtake the
 * queue. probe-rs computes its depth as the reported count minus one, so a probe advertising two
 * gets no overlap at all.
 *
 * The receive endpoint is armed only while a slot is free, which is what refuses a command there
 * is nowhere to put: the host sees NAK and retries, rather than the firmware dropping a packet.
 */
#ifndef REQUEST_SLOTS
#define REQUEST_SLOTS 2U
#endif

// Word aligned, which no compiler is obliged to make a byte array: the generated transfers store
// words into the reply, and the copy into the endpoint buffer moves words only when both ends are.
static uint8_t request[REQUEST_SLOTS][DAP_PACKET_SIZE] __attribute__((aligned(4)));
static volatile uint16_t request_len[REQUEST_SLOTS];
static uint8_t request_head; // the slot dap_task takes next
static uint8_t request_tail; // the slot tud_vendor_rx_cb fills next

static uint8_t response[DAP_RESPONSE_SIZE] __attribute__((aligned(4)));
/** Non-zero while a reply is built and the transmit endpoint has not taken it yet. */
static uint32_t response_len;

// CMSIS-DAP v1 shares the command layer with v2 but not its packet size: a HID report is bounded
// by the interrupt endpoint while the bulk interface carries 512. The buffer is full size so an
// over-long response cannot run past it; only a report's worth is ever sent.
static uint8_t hid_response[DAP_RESPONSE_SIZE] __attribute__((aligned(4)));

#ifdef MARKER_PINS
/** Which J8 pins the marker still owns. ID_VENDOR_J8 takes pins away from it and gives them back. */
volatile uint32_t marker_mask = J8_PIN_MASK;
#endif

/** One received packet, which is one command. Runs inside tud_task, not alongside it. */
void tud_vendor_rx_cb(uint8_t index, const uint8_t* buffer, uint32_t length) {
    (void)index;
    if ((length == 0U) || (length > DAP_PACKET_SIZE)) {
        return; // dap_task arms the endpoint again
    }
    // **The abort is handled here and never queued.** It exists to stop a transfer that is
    // running now, so putting it in a slot behind that transfer makes it useless: the flag would
    // be set once the thing it was meant to abort had already finished. ARM's own MCU-Link
    // example does the same in its endpoint callback. It answers nothing, which is why a host
    // that reads for a reply after sending it waits for ever.
    //
    // This is also the contract a host relies on when it keeps its pipeline one packet short of
    // what the probe says it holds: the spare buffer is for an abort to arrive in, and it only
    // helps if the abort overtakes the queue rather than joining it.
    if (buffer[0] == ID_DAP_TransferAbort) {
        DAP_TransferAbort = 1U;
        return;
    }
    memcpy(request[request_tail], buffer, length);
    request_len[request_tail] = (uint16_t)length;
    request_tail = (uint8_t)((request_tail + 1U) % REQUEST_SLOTS);
}

static void dap_task(void) {
    // Arm the receive endpoint whenever a slot can take a command. Idempotent: the call claims
    // the endpoint and does nothing if a transfer is already up, so this also covers the first
    // arming after enumeration.
    if (request_len[request_tail] == 0U) {
        tud_vendor_read_xfer();
    }

    // A reply the endpoint would not take last time. Nothing else may run until it goes, because
    // a host reads replies in the order it sent the commands.
    if (response_len != 0U) {
        if (tud_vendor_write(response, response_len) != response_len) {
            return;
        }
        response_len = 0U;
    }

    if (request_len[request_head] == 0U) {
        return;
    }

    // The high half of the return value counts request bytes consumed; only the response length
    // matters here, because one packet carries one command.
    led_activity();
    MARKER_BUSY();
    uint32_t produced = DAP_ExecuteCommand(request[request_head], response) & 0xFFFFU;
    MARKER_IDLE();
    // One command answers in one packet. A longer reply would split across two, which a v2 host
    // does not expect, so send what fits and drop the rest with the malformed request that caused
    // it.
    if (produced > DAP_PACKET_SIZE) {
        produced = DAP_PACKET_SIZE;
    }
    request_len[request_head] = 0U;
    request_head = (uint8_t)((request_head + 1U) % REQUEST_SLOTS);

    if (produced != 0U) {
        response_len = produced;
        if (tud_vendor_write(response, response_len) == response_len) {
            response_len = 0U;
        }
    }
}

/** Answer the v1 host with the v1 packet size.
 *
 * DAP_Info builds its reply from DAP_PACKET_SIZE, which is sized for the bulk interface. A v1 host
 * that read 512 would try to send a 512-byte HID report, which cannot exist. The command layer has
 * no idea which interface asked, so the correction belongs here.
 */
static void hid_report_packet_size(const uint8_t* req, uint8_t* resp) {
    if ((req[0] == ID_DAP_Info) && (req[1] == DAP_ID_PACKET_SIZE)) {
        resp[1] = 2U;
        resp[2] = (uint8_t)(CFG_TUD_HID_EP_BUFSIZE & 0xFFU);
        resp[3] = (uint8_t)(CFG_TUD_HID_EP_BUFSIZE >> 8);
    }
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t* buffer,
                               uint16_t reqlen) {
    // Requests arrive as OUT reports, never as GET_REPORT.
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const* buffer,
                           uint16_t bufsize) {
    (void)report_id;
    (void)report_type;

    if (bufsize == 0U) {
        return;
    }

    led_activity();
    DAP_ExecuteCommand(buffer, hid_response);
    hid_report_packet_size(buffer, hid_response);
    tud_hid_n_report(instance, 0U, hid_response, CFG_TUD_HID_EP_BUFSIZE);
}

int main(void) {
    board_init();
    reboot_clear_boot_mode();
    board_core_clock_init();
    board_clocks_init();
    board_timestamp_init();
    DAP_Setup();
    target_sense_init();
    led_init();
    vcom_init();

    board_init_after_tusb();
    tusb_rhport_init_t const init = {
        .role = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_AUTO,
    };
    tusb_init(BOARD_TUD_RHPORT, &init);

    for (;;) {
        tud_task();
        dap_task();
        vcom_task();
        reboot_poll();
    }
}
