/** CPU 0's side of Verge Aux (HOST-INTERFACE.md, "Verge Aux"): the interface's two endpoints, and
 *  the slot each request goes to CPU 1 in (src/aux_slot.h). */
#ifndef AUX_H
#define AUX_H

#include <stdbool.h>
#include <stdint.h>

/** Whether this configuration has the interface, and which TinyUSB vendor instance it is. Call
 *  before tusb_init. */
void aux_init(bool present, uint8_t vendor_index);

/** Whether a vendor callback for `vendor_index` is the aux interface's. */
bool aux_owns(uint8_t vendor_index);

/** The vendor callbacks, for the aux interface's instance. */
void aux_received(const uint8_t* buffer, uint32_t length);
void aux_sent(void);

/** At each configuration the host sets: transfers TinyUSB had started are gone, and a reply to an
 *  earlier request is a gone host's. */
void aux_mount(void);

/** Send a reply CPU 1 has left, and arm OUT for the next request once the slot is free. */
void aux_task(void);

/** Whether aux_task has something to do that no interrupt or mailbox raise will announce again. */
bool aux_has_work(void);

#endif
