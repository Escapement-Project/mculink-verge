/** A bench marker on J8, the four-pin header beside the MCU.
 *
 * The wire is 66% of a block read and USB and the host are the other 34%, which until now was an
 * arithmetic residual: total time measured on the host, wire time measured on the analyser, the
 * rest by subtraction. A pin that goes high while the firmware runs a command and low otherwise
 * puts the gap on the same timeline as the clock, so the two are read from one capture and the
 * subtraction goes away. It is also a hard trigger, which replaces scanning SWDIO for a request
 * pattern to align a capture.
 *
 * **All four pins move together**, because `GPIO->SET[1]` takes a mask and one store sets four
 * pins as cheaply as one. That makes the header orientation-free: clip onto whichever pad is
 * convenient. ID_VENDOR_J8 hands individual pins to the host, which is how a pad gets identified.
 *
 * J8 is PIO1_0 to PIO1_3 on the LPC55S69 and nothing else on the board touches it: no buffer, no
 * series resistor, no ground pin. It is the only free header there is.
 *
 * The cost is one store on each side of a command, once per command and never in a bit path. A
 * data bit is fifteen cycles and a command is thousands, so this does not move any figure it is
 * used to measure. It is still behind an option, because a probe in someone else's hands should
 * not drive pins for a reason that only makes sense at a bench.
 */
#ifndef MARKER_H
#define MARKER_H

#include "board_pins.h"

#ifdef MARKER_PINS

#include <stdint.h>

/** Which of the four pins the marker still owns. ID_VENDOR_J8 takes pins away from it. */
extern volatile uint32_t marker_mask;

#define MARKER_BUSY() (J8_GPIO->SET[J8_PORT] = marker_mask)
#define MARKER_IDLE() (J8_GPIO->CLR[J8_PORT] = marker_mask)

#else

#define MARKER_BUSY() ((void)0)
#define MARKER_IDLE() ((void)0)

#endif

#endif
