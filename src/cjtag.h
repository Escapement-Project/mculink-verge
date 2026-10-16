/** IEEE 1149.7 two-pin (cJTAG) support, OScan1 format.
 *
 * Enabled by a vendor command, because a probe cannot tell from the wire whether a target is
 * four-pin or two-pin, and driving the activation sequence at a four-pin target would disturb it.
 */
#ifndef CJTAG_H
#define CJTAG_H

#include <stdbool.h>
#include <stdint.h>

/** Whether the TAP is being driven as OScan1 rather than four-wire JTAG. */
bool cJTAG_Active(void);

/** Drive the online and OScan1 activation sequence, and mark the TAP as two-pin.
 *
 * Drives the wire; does not configure the pins. DAP_Connect has to have run, or the pins are
 * still inputs and the sequence goes nowhere without saying so.
 *
 * Safe to call when already active: the sequence begins from a TAP reset.
 */
void cJTAG_Activate(void);

/** Return to four-wire JTAG without touching the wire.
 *
 * The target stays in OScan1; this only stops the probe framing for it. Getting the target back is
 * a power-on reset, so this exists for the host to undo its own mode switch, not to undo the
 * target's.
 */
void cJTAG_Deactivate(void);

/** One OScan1 frame: the three TMSC packets that carry one JTAG clock.
 *
 * nTDI then TMS driven by us, then TDO with the wire released. Returns the TDO bit.
 *
 * Entered and left with TCKC low and the wire driven, which is what cJTAG_Activate leaves behind.
 *
 * Two copies, paced and not. The caller picks once per transfer; see the note in cjtag.c for why
 * it cannot be picked per frame.
 */
uint32_t cJTAG_FrameSlow(uint32_t tms, uint32_t tdi);
uint32_t cJTAG_FrameFast(uint32_t tms, uint32_t tdi);

#endif
