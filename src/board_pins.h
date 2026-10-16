/** MCU-Link debug interface pin map.
 *
 * The names match the board header in ARM's CMSIS-DAP MCU-Link example, so the pin layer in
 * DAP_config.h compiles against this unchanged. The map follows the schematic's net names.
 *
 * Both buffer-direction pins are outputs the firmware owns. Releasing SWDIO means clearing
 * TMS_SWDIO_TXEN as well as making the pin an input: the buffer keeps driving the wire otherwise.
 */
#ifndef BOARD_PINS_H
#define BOARD_PINS_H

#define DBGIF_TCK_SWCLK_GPIO            GPIO
#define DBGIF_TCK_SWCLK_PORT            0U
#define DBGIF_TCK_SWCLK_PIN             0U

#define DBGIF_TDI_GPIO                  GPIO
#define DBGIF_TDI_PORT                  0U
#define DBGIF_TDI_PIN                   1U

#define DBGIF_TMS_SWDIO_GPIO            GPIO
#define DBGIF_TMS_SWDIO_PORT            0U
#define DBGIF_TMS_SWDIO_PIN             2U

#define DBGIF_TDO_SWO_GPIO              GPIO
#define DBGIF_TDO_SWO_PORT              0U
#define DBGIF_TDO_SWO_PIN               3U

#define DBGIF_TMS_SWDIO_TXEN_GPIO       GPIO
#define DBGIF_TMS_SWDIO_TXEN_PORT       0U
#define DBGIF_TMS_SWDIO_TXEN_PIN        28U

#define DBGIF_RESET_GPIO                GPIO
#define DBGIF_RESET_PORT                0U
#define DBGIF_RESET_PIN                 19U

#define DBGIF_RESET_TXEN_GPIO           GPIO
#define DBGIF_RESET_TXEN_PORT           0U
#define DBGIF_RESET_TXEN_PIN            13U

#define LED1_GPIO                       GPIO
#define LED1_PORT                       0U
#define LED1_PIN                        5U

/** Put the debug interface pins into their idle state.
 *
 * Called from DAP_SETUP, so it runs before any host command can reach the pin layer.
 */
void board_debug_pins_init(void);

#endif
