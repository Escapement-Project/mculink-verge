/** What the onboard LED says, and who is allowed to say it.
 *
 * The board calls it "SWD activity" (LED1 on P0_5, red, through 2K to 3V3, so a low pin is lit)
 * but CMSIS-DAP has no notion of activity. Its only LED command is DAP_HostStatus, which carries
 * the host's opinion of whether a debugger is attached. So the probe decides for itself:
 *
 *  - breathing: powered, and nothing is using it. The stock firmware does the same.
 *  - blinking: commands are arriving.
 *  - lit: a host has said it is connected, and is not using the wire.
 *
 * Activity wins while it lasts, because it answers the question someone asks when they look at a
 * probe: is this the one that is working?
 *
 * **The LED is a timer's, not the main loop's.** CTIMER3 drives the pin as a PWM and its period
 * interrupt runs the policy, so a command pays for one byte store and nothing else. An earlier
 * version polled from the main loop between commands and cost a measurable fraction of block
 * throughput doing it, because that is exactly where the next command waits.
 */
#ifndef LED_H
#define LED_H

#include <stdint.h>

/** Set by every command and taken by the LED's tick. A lost update loses one command's worth of
 *  blinking, so neither side locks: a command must not pay for the LED. */
extern volatile uint8_t led_activity_seen;

/** A command has arrived. One store, so it may go anywhere a command passes, never in a bit path. */
static inline void led_activity(void) {
    led_activity_seen = 1U;
}

/** The host's opinion of whether it is connected, from DAP_HostStatus. */
void led_connected(uint32_t on);

/** Hand the pin to CTIMER3 and start it. FRO_HF clocks the timer, so call after
 *  board_clocks_init. */
void led_init(void);

#endif
