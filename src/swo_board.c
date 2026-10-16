/** Board side of SWO capture.
 *
 * Trace arrives on P0_3, which is also JTAG's TDO. The pin therefore has two configurations and
 * only one of them can be live: GPIO input while the TAP is being scanned, FC3_RXD while trace is
 * being captured. NXP's CMSIS-Driver calls the two hooks below from its Initialize and
 * Uninitialize, which is where ARM's SWO.c turns capture on and off, so the switch lands exactly
 * at the mode change rather than being left to whoever remembers.
 */
#include "fsl_clock.h"
#include "fsl_device_registers.h"
#include "fsl_iocon.h"

#include "board_pins.h"
#include "swo_board.h"

#define SWO_PIN_FLEXCOMM (IOCON_FUNC1 | IOCON_MODE_INACT | IOCON_DIGITAL_EN | IOCON_SLEW_FAST)
#define SWO_PIN_GPIO (IOCON_FUNC0 | IOCON_MODE_INACT | IOCON_DIGITAL_EN)

uint32_t USART3_GetFreq(void) { return CLOCK_GetFlexCommClkFreq(3U); }

void board_swo_pin_init(void) {
    // The divided high-speed FRO rather than the 12 MHz one: SWO runs at megabaud rates and a
    // 12 MHz reference cannot divide to them with usable error.
    CLOCK_AttachClk(kFRO_HF_DIV_to_FLEXCOMM3);
    IOCON_PinMuxSet(IOCON, DBGIF_TDO_SWO_PORT, DBGIF_TDO_SWO_PIN, SWO_PIN_FLEXCOMM);
}

void board_swo_pin_deinit(void) {
    IOCON_PinMuxSet(IOCON, DBGIF_TDO_SWO_PORT, DBGIF_TDO_SWO_PIN, SWO_PIN_GPIO);
    GPIO->DIRCLR[DBGIF_TDO_SWO_PORT] = (1UL << DBGIF_TDO_SWO_PIN);
    CLOCK_AttachClk(kNONE_to_FLEXCOMM3);
}
