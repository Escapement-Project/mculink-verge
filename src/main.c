/** CMSIS-DAP firmware for the MCU-Link.
 *
 * One DAP command per USB packet, which is what CMSIS-DAP v2 specifies: the host sends a request
 * and waits for exactly one response before sending the next. There is no queue here because the
 * protocol does not need one -- DAP_ExecuteCommands batches on the host's behalf inside a single
 * packet instead.
 */
#include "bsp/board_api.h"
#include "tusb.h"

#include "board_pins.h"
#include "vcom.h"

// DAP_config.h first: DAP.h reads the DAP_SWD and DAP_JTAG settings it defines, so a sorted
// include block that puts DAP.h ahead of it will not compile.
#include "DAP_config.h"

#include "DAP.h"

static uint8_t request[DAP_PACKET_SIZE];
static uint8_t response[DAP_PACKET_SIZE];

// CMSIS-DAP v1 shares the command layer with v2 but not its packet size: a HID report is bounded
// by the interrupt endpoint while the bulk interface carries 512. The buffer is full size so an
// over-long response cannot run past it; only a report's worth is ever sent.
static uint8_t hid_response[DAP_PACKET_SIZE];

static void dap_task(void) {
    if (!tud_vendor_available()) {
        return;
    }

    uint32_t received = tud_vendor_read(request, sizeof(request));
    if (received == 0) {
        return;
    }

    // The high half of the return value counts request bytes consumed; only the response length
    // matters here, because one packet carries one command.
    uint32_t produced = DAP_ExecuteCommand(request, response) & 0xFFFFU;
    if (produced != 0) {
        tud_vendor_write(response, produced);
        tud_vendor_flush();
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

    DAP_ExecuteCommand(buffer, hid_response);
    hid_report_packet_size(buffer, hid_response);
    tud_hid_n_report(instance, 0U, hid_response, CFG_TUD_HID_EP_BUFSIZE);
}

int main(void) {
    board_init();
    board_clocks_init();
    board_timestamp_init();
    DAP_Setup();
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
    }
}
