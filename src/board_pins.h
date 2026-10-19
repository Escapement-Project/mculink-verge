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

#define LED1_PORT 0U
#define LED1_PIN 5U

/** J8, the four-pin 50-mil header beside the MCU: PIO1_0 to PIO1_3, J8 pin 1 to pin 4.
 *
 * The only pins on this board that nothing else uses. No buffer, no series resistor and no ground
 * pin on the header, so an analyser takes its ground from the debug connector. src/marker.h says
 * what they are for.
 */
#define J8_GPIO GPIO
#define J8_PORT 1U
#define J8_PIN_MASK 0x0FU

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

/** CPU 1: put the debug interface pins into their idle state.
 *
 * Called from DAP_SETUP, so it runs before any host command can reach the pin layer.
 */
void board_debug_pins_init(void);

#endif
