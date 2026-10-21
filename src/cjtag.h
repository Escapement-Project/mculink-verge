/** IEEE 1149.7 two-pin (cJTAG) support, OScan1 and OScan2 formats.
 *
 * Enabled by a vendor command, because a probe cannot tell from the wire whether a target is
 * four-pin or two-pin, and driving the activation sequence at a four-pin target would disturb it.
 */
#ifndef CJTAG_H
#define CJTAG_H

#include <stdbool.h>
#include <stdint.h>

/** Scan formats, numbered as the standard's SCNFMT register and its STFMT command number them. */
#define CJTAG_OSCAN1 9U
#define CJTAG_OSCAN2 10U
/** The formats this probe frames, bit n for SCNFMT n. */
#define CJTAG_FORMATS ((1UL << CJTAG_OSCAN1) | (1UL << CJTAG_OSCAN2))

/** TAP controller states, in the numbering ID_VENDOR_CJTAG_CONTROL mode 3 takes them. */
enum cjtag_tap {
    CJTAG_TAP_RESET,
    CJTAG_TAP_IDLE,
    CJTAG_TAP_SELECT_DR,
    CJTAG_TAP_CAPTURE_DR,
    CJTAG_TAP_SHIFT_DR,
    CJTAG_TAP_EXIT1_DR,
    CJTAG_TAP_PAUSE_DR,
    CJTAG_TAP_EXIT2_DR,
    CJTAG_TAP_UPDATE_DR,
    CJTAG_TAP_SELECT_IR,
    CJTAG_TAP_CAPTURE_IR,
    CJTAG_TAP_SHIFT_IR,
    CJTAG_TAP_EXIT1_IR,
    CJTAG_TAP_PAUSE_IR,
    CJTAG_TAP_EXIT2_IR,
    CJTAG_TAP_UPDATE_IR,
    CJTAG_TAP_STATES
};

/** Whether the TAP is being driven over two pins rather than four-wire JTAG. */
bool cJTAG_Active(void);

/** The scan format two-pin frames take, CJTAG_OSCAN1 or CJTAG_OSCAN2. */
uint32_t cJTAG_Format(void);

/** OScan2's frame depends on the TAP state the frame is clocked in, so the probe follows the TAP
 * from the TMS it sends. Every OScan2 frame has to go through cJTAG_TapClock, or the next frame
 * takes the wrong shape and the target loses the frame boundaries. A clock the probe does not
 * frame -- DAP_SWJ_Sequence, DAP_SWD_Sequence, DAP_SWJ_Pins, DAP_Connect -- moves the target's
 * TAP and not this one, so a host that sends one owes ID_VENDOR_CJTAG_CONTROL mode 3 again. */
uint32_t cJTAG_Tap(void);
/** Moves the tracked TAP by one clock with TMS `tms`, returning whether that clock was in a Shift
 * state, where an OScan2 frame is three packets rather than one. */
bool cJTAG_TapClock(uint32_t tms);

/** Drive the online and OScan1 activation sequence, and mark the TAP as two-pin.
 *
 * Drives the wire; does not configure the pins. DAP_Connect has to have run, or the pins are
 * still inputs and the sequence goes nowhere without saying so.
 *
 * Safe to call when already active: the sequence begins from a TAP reset.
 */
void cJTAG_Activate(void);

/** Mark the TAP as two-pin in `format` without driving any activation, for a host that has driven
 * its own and left the TAP in `tap`, which only OScan2 needs.
 *
 * Lowers the clock and takes the wire at the level it already holds, which is the state every
 * frame starts from. Nothing else moves, so the target sees no edge it was not sent. False, and
 * nothing changed, for a format this probe does not frame or a state that does not exist.
 */
bool cJTAG_Enter(uint32_t format, uint32_t tap);

/** Return to four-wire JTAG.
 *
 * The target stays in its two-pin format; this only stops the probe framing for it. Getting the
 * target back is a power-on reset, so this exists for the host to undo its own mode switch, not to
 * undo the target's. In OScan2 one more frame goes out first, where it cannot move the TAP: the
 * last frame outside Shift advances the target's TAP only at the next TCKC period (1149.7
 * 25.6.2), so without it a host's walk into Test-Logic-Reset stops one state short.
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

/** One OScan2 frame, paced: OScan1's three packets in a Shift state, and outside one a single TMS
 * packet, for which the target sends nothing back and this returns 1, the level a released wire
 * reads there. Moves the tracked TAP. */
uint32_t cJTAG_Frame2(uint32_t tms, uint32_t tdi);

#endif
