/** The Pro's four digital pins on J19, DIO0 to DIO3, as GPIO the host drives and reads.
 *
 * Each pin reaches the header through a level translator whose direction a pin of ours sets:
 * IO_DIR1 turns DIO0, DIO1 and DIO2 round together, IO_DIR2 turns DIO3 (Pro netlist). A 1K
 * pull-down on each makes all four inputs until the firmware says otherwise. A direction is
 * therefore a group's, and a request that would split a group is refused.
 *
 * Nothing here knows the transport: ID_VENDOR_DIO (dap_vendor.c) is one front end, and USB-SIO
 * would be another.
 *
 * The state outlives sessions, so a host can hold a target's boot pin through a reconnect. Only a
 * reset of the probe puts the pins back to inputs. CPU 1 only.
 */
#ifndef DIO_H
#define DIO_H

#include <stdbool.h>
#include <stdint.h>

#define DIO_GROUPS_MAX 2U

typedef struct {
    uint8_t pins;    // the pins this board has and its jumpers leave on, bit n DIOn
    uint8_t outputs; // the pins the probe drives
    uint8_t levels;  // each pin's output latch: what it drives, or will once it is an output
    uint8_t read;    // the probe's side of each translator, now, so an output reads our own drive
    uint8_t groups;  // how many of group[] are used
    uint8_t group[DIO_GROUPS_MAX]; // the pins that turn round together
} dio_state_t;

/** The pins to inputs and their directions toward the probe, on a Pro without J15 (USB-SIO off)
 *  fitted; elsewhere nothing. Call after board_debug_pins_init, which sets board_kind. */
void dio_init(void);

/** Whether this board has the pins and its jumpers leave them on. */
bool dio_available(void);

/** Sets the directions and output levels of the pins in `mask`, leaving the others as they are.
 *  Returns false, changing nothing, when the pins are not available, `mask` names a pin they do not
 *  have, or the result would split a group. A mask of 0 changes nothing and succeeds wherever the
 *  pins are available. */
bool dio_set(uint8_t mask, uint8_t outputs, uint8_t levels);

void dio_get(dio_state_t* state);

#endif
