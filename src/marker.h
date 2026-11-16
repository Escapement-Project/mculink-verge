/** A bench marker on J8, the plain board's DIO pins (src/dio.h).
 *
 * Without it, the time a block read spends off the wire is an arithmetic residual: total time
 * measured on the host, wire time measured on the analyser, the rest by subtraction. A pin that goes
 * high while the firmware runs a command and low otherwise puts the gap on the same timeline as the
 * clock, so the two are read from one capture and the subtraction goes away. It is also a hard
 * trigger, which replaces scanning SWDIO for a request pattern to align a capture.
 *
 * It is an internal testing feature. At boot it holds no pins, and J8 is four DIO inputs.
 * ID_VENDOR_J8 gives it pins, which stop being DIO pins until it gives them back (src/dio.c).
 * **The pins it holds move together**, because `GPIO->SET[1]` takes a mask and one store sets four
 * pins as cheaply as one. Given all four, the header is orientation-free: clip onto whichever pad
 * is convenient.
 *
 * The cost is one store on each side of a command, made whether the marker holds pins or not,
 * once per command and never in a bit path. A data bit is fifteen cycles and a command is
 * thousands, so this does not move any figure it is used to measure. `MARKER` is on by default,
 * so the image a user runs is the one the bench measured.
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
