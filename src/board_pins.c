#include "fsl_device_registers.h"
#include "fsl_iocon.h"
#include "board_pins.h"

/** Digital GPIO, no pull, standard slew: what every debug pin wants.
 *
 * The buffers on this board give the drive strength, so nothing here needs a pull resistor. The
 * two TXEN pins are the exception that matters: they must come up low, because a buffer enabled
 * before the firmware knows the port state would drive the target's wire.
 */
#define DEBUG_PIN_MODE (IOCON_FUNC0 | IOCON_MODE_INACT | IOCON_DIGITAL_EN)

void board_debug_pins_init(void) {
  CLOCK_EnableClock(kCLOCK_Iocon);
  CLOCK_EnableClock(kCLOCK_Gpio0);

  static const uint32_t pins[] = {
      DBGIF_TCK_SWCLK_PIN, DBGIF_TDI_PIN,  DBGIF_TMS_SWDIO_PIN,      DBGIF_TDO_SWO_PIN,
      DBGIF_RESET_PIN,     LED1_PIN,       DBGIF_TMS_SWDIO_TXEN_PIN, DBGIF_RESET_TXEN_PIN,
  };
  for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
    IOCON_PinMuxSet(IOCON, DBGIF_TCK_SWCLK_PORT, (uint8_t)pins[i], DEBUG_PIN_MODE);
  }

  // Buffers off first, then the pins they gate. Doing it the other way round drives the target
  // for however long the two writes are apart.
  GPIO->CLR[DBGIF_TMS_SWDIO_TXEN_PORT] = (1UL << DBGIF_TMS_SWDIO_TXEN_PIN);
  GPIO->CLR[DBGIF_RESET_TXEN_PORT] = (1UL << DBGIF_RESET_TXEN_PIN);
  GPIO->DIRSET[DBGIF_TMS_SWDIO_TXEN_PORT] =
      (1UL << DBGIF_TMS_SWDIO_TXEN_PIN) | (1UL << DBGIF_RESET_TXEN_PIN);

  // Everything that talks to the target starts as an input, so connecting the probe to a running
  // board disturbs nothing until the host asks for a port.
  GPIO->DIRCLR[DBGIF_TCK_SWCLK_PORT] = (1UL << DBGIF_TCK_SWCLK_PIN) | (1UL << DBGIF_TDI_PIN) |
                                       (1UL << DBGIF_TMS_SWDIO_PIN) | (1UL << DBGIF_TDO_SWO_PIN) |
                                       (1UL << DBGIF_RESET_PIN);

  GPIO->SET[LED1_PORT] = (1UL << LED1_PIN);  // active low, so set is off
  GPIO->DIRSET[LED1_PORT] = (1UL << LED1_PIN);
}
