/** CMSIS-DAP firmware for the MCU-Link.
 *
 * One DAP command per USB packet, which is what CMSIS-DAP v2 specifies: the host sends a request
 * and waits for exactly one response before sending the next. There is no queue here because the
 * protocol does not need one -- DAP_ExecuteCommands batches on the host's behalf inside a single
 * packet instead.
 */
#include "bsp/board_api.h"
#include "tusb.h"

#include "vcom.h"

#include "DAP_config.h"
#include "DAP.h"

static uint8_t request[DAP_PACKET_SIZE];
static uint8_t response[DAP_PACKET_SIZE];

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

int main(void) {
  board_init();
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
