/** What the LEDs say, and who is allowed to say it.
 *
 * Every board has the status LED (P0_5, red, through 2K to 3V3, so a low pin is lit). The board
 * calls it "SWD activity", but CMSIS-DAP has no notion of activity. Its only LED command is
 * DAP_HostStatus, which carries the host's opinion of whether a debugger is attached. So the probe
 * decides for itself:
 *
 *  - breathing: powered, and no host has said it is connected. The stock firmware does the same.
 *  - lit: a host has said it is connected.
 *  - a flicker: a command has arrived. The LED inverts for a moment, as a network LED does, and
 *    a stream of commands flickers it steadily.
 *
 * The Pro has six more, lit by a low pin the same way (PRO.md):
 *
 *  - LED1 USB: lit while configured and not suspended.
 *  - LED2 SWO and LED4 VCOM: dark, flickering on traffic: SWO bytes going to the host, console
 *    bytes either way on either console.
 *  - LED5 FUNC: the wire in use, by the shape of a repeating ramp. SWD rises from dark to light,
 *    two-pin cJTAG falls from light to dark, four-wire JTAG goes light to dark to light. Solid with
 *    the SWD jumper fitted, dark with no port connected.
 *  - LED6 SIO: lit while the host drives any of J19's DIO pins or runs SPI through those pins.
 *  - LED7 NRG: no indication is driven.
 *
 * Every fade moves evenly in CIE lightness (L*), so it looks even to the eye.
 *
 * **The LEDs are a timer's, not the main loop's.** CTIMER3's period interrupt on CPU 0 runs the
 * policy, so a command pays for one byte store and nothing else. An earlier version polled from the
 * main loop between commands and cost a measurable fraction of block throughput doing it, because
 * that is exactly where the next command waits.
 */
#ifndef LED_H
#define LED_H

#include <stdint.h>

#include "dap_shared.h"

/** The wire in use, for the FUNC LED. */
#define LED_WIRE_NONE 0U
#define LED_WIRE_SWD 1U
#define LED_WIRE_JTAG 2U
#define LED_WIRE_CJTAG 3U

/** CPU 1: a command has arrived. Taken by CPU 0's LED tick. A lost update loses one flicker, so
 *  neither side locks: a command must not pay for the LED. One store, so it may go anywhere a
 *  command passes, never in a bit path. */
static inline void led_activity(void) {
    dap_shared.led_activity = 1U;
}

/** CPU 1: the host's opinion of whether it is connected, from DAP_HostStatus. */
static inline void led_connected(uint32_t on) {
    dap_shared.led_connected = (on != 0U);
}

/** CPU 1: the wire in use, LED_WIRE_*. */
static inline void led_wire(uint8_t wire) {
    dap_shared.led_wire = wire;
}

/** CPU 1: SWO bytes have gone to the host. */
static inline void led_swo(void) {
    dap_shared.led_swo = 1U;
}

/** CPU 1: whether the host drives any DIO pin. */
static inline void led_sio(uint32_t on) {
    dap_shared.led_sio = (on != 0U);
}

/** CPU 0: console bytes have moved, set by src/vcom.c and taken by the LED tick. */
extern volatile uint8_t led_vcom_seen;

static inline void led_vcom(void) {
    led_vcom_seen = 1U;
}

/** CPU 0: hand the LED pins to their timers and GPIO, and start the tick. FRO_HF clocks the
 *  timers, so call after board_clocks_init; the Pro's LEDs need the board, so after
 *  board_read_straps. */
void led_init(void);

#endif
