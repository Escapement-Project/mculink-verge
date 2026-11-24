/** MCU-Link board bring-up: the debug interface pin map, and the clocks the BSP leaves off.
 *
 * The pin names match the board header in ARM's CMSIS-DAP MCU-Link example, so the pin layer in
 * DAP_config.h compiles against this unchanged. The map follows the schematic's net names.
 *
 * Both buffer-direction pins are outputs the firmware owns. Releasing SWDIO means clearing
 * TMS_SWDIO_TXEN as well as making the pin an input: the buffer keeps driving the wire otherwise.
 */
#ifndef BOARD_PINS_H
#define BOARD_PINS_H

#include <stdbool.h>
#include <stdint.h>

#include "board_straps.h"

/** Every signal below is on port 0, which is what lets the init code write them a port at a time.
 * board_pins.c holds a static assertion for it. */
#define DBGIF_PORT 0U

/** The GPIO block the debug interface is driven through: the secure GPIO, which drives port 0 pins
 * set to IOCON function 10 and has port 0's register layout at its own base (UM11126 chapter 17).
 *
 * It sits on AHB matrix port 10. The ordinary GPIO shares port 8 with Flexcomm 0 to 4 and the
 * mailbox (UM11126 table 6), and CPU 0's console polling there delayed CPU 1's pin accesses by
 * enough to misread bits at 50 MHz. */
#define DBGIF_GPIO SECGPIO

#define DBGIF_TCK_SWCLK_GPIO DBGIF_GPIO
#define DBGIF_TCK_SWCLK_PORT 0U
#define DBGIF_TCK_SWCLK_PIN 0U

#define DBGIF_TDI_GPIO DBGIF_GPIO
#define DBGIF_TDI_PORT 0U
#define DBGIF_TDI_PIN 1U

#define DBGIF_TMS_SWDIO_GPIO DBGIF_GPIO
#define DBGIF_TMS_SWDIO_PORT 0U
#define DBGIF_TMS_SWDIO_PIN 2U

#define DBGIF_TDO_SWO_GPIO DBGIF_GPIO
#define DBGIF_TDO_SWO_PORT 0U
#define DBGIF_TDO_SWO_PIN 3U

#define DBGIF_TMS_SWDIO_TXEN_GPIO DBGIF_GPIO
#define DBGIF_TMS_SWDIO_TXEN_PORT 0U
#define DBGIF_TMS_SWDIO_TXEN_PIN 28U

#define DBGIF_RESET_GPIO DBGIF_GPIO
#define DBGIF_RESET_PORT 0U
#define DBGIF_RESET_PIN 19U

#define DBGIF_RESET_TXEN_GPIO DBGIF_GPIO
#define DBGIF_RESET_TXEN_PORT 0U
#define DBGIF_RESET_TXEN_PIN 13U

/** The target's ISP pin, debug connector pin 7, through an open-drain buffer (74LVC1G07) on the
 *  plain board and the Pro: a low pin pulls the target's ISP pin low. NXP's parts sample theirs at
 *  reset, so left low it boots an attached LPC into its bootloader. Held high, released.
 *
 * Until CPU 1 sets it up, and while the probe is in its own bootloader, the pin is an input. The
 * Pro pulls the buffer's input up (R89); the plain board's schematic shows no pull-up, so there the
 * target's ISP pin is not held released in those windows. */
#define DBGIF_ISP_CTRL_PORT 0U
#define DBGIF_ISP_CTRL_PIN 14U

#define LED1_PORT 0U
#define LED1_PIN 5U

/** CPU 1: which board this is (board_straps.h), as CPU 0 read it, copied from dap_shared by
 *  board_debug_pins_init. */
extern uint8_t board_kind;

/** CPU 0: start the clocks the peripherals here depend on.
 *
 * Kept out of line, like the other two things that run once at boot and bring a peripheral up.
 * The call costs nothing at that rate, and being a symbol is what lets tools/image.py run
 * it and ask the clock driver what came out.
 *
 * Call before anything attaches a Flexcomm, and before CPU 1 starts. See the note in
 * board_clocks.c for why this is not something the board support already did.
 */
void board_clocks_init(void);

/** CPU 1: start the cycle counter that TIMESTAMP_GET reads. Each core has its own DWT, so this
 *  runs on the core that reads it. Call before the first host command. */
void board_timestamp_init(void);

/** CPU 1: take the board from dap_shared, and put the debug interface pins into their idle state.
 *
 * Called from DAP_SETUP, so it runs before any host command can reach the pin layer. Out of line,
 * so tools/image.py can run it for each board.
 */
void board_debug_pins_init(void);

/** CPU 1: give TDO/SWO to Flexcomm 3 for SWO capture, or back to the secure GPIO as JTAG's TDO. */
void board_swo_pin(bool capture);

#endif
