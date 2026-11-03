/** IEEE 1149.7 (cJTAG) support, OScan1, OScan2, OScan5 and OScan6 formats.
 *
 * Enabled by a vendor command, because a probe cannot tell from the wire whether a target
 * speaks JTAG or cJTAG. The host drives the target's activation, then turns framing on.
 */
#ifndef CJTAG_H
#define CJTAG_H

#include <stdbool.h>
#include <stdint.h>

/** Scan formats, numbered as the standard's SCNFMT register and its STFMT command number them. */
#define CJTAG_OSCAN1 9U
#define CJTAG_OSCAN2 10U
#define CJTAG_OSCAN5 13U
#define CJTAG_OSCAN6 14U
/** The formats this probe frames, bit n for SCNFMT n. */
#define CJTAG_FORMATS \
    ((1UL << CJTAG_OSCAN1) | (1UL << CJTAG_OSCAN2) | (1UL << CJTAG_OSCAN5) | (1UL << CJTAG_OSCAN6))

/** The formats offered a host, in vendor 0x82 and to ID_VENDOR_CJTAG_CONTROL mode 3: CJTAG_FORMATS
 * until cJTAG_Offer limits them, which lasts until the probe restarts. */
uint32_t cJTAG_Offered(void);
/** Offers the formats in `mask` that this probe frames, and OScan1 whatever the mask says, so that
 * a person can keep a host to OScan1 while looking at the wire. A format already framed stays. */
void cJTAG_Offer(uint32_t mask);

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

/** Whether the TAP is being driven over cJTAG rather than JTAG. */
bool cJTAG_Active(void);

/** The scan format cJTAG frames take, CJTAG_OSCAN1, _OSCAN2, _OSCAN5 or _OSCAN6. */
uint32_t cJTAG_Format(void);

/** OScan2's, OScan5's and OScan6's frames depend on the TAP state the frame is clocked in, so the probe
 * follows the TAP from the TMS it sends. Every frame in those formats has to go through
 * cJTAG_TapClock, or the next frame takes the wrong shape and the target loses the frame
 * boundaries. A clock the probe does not
 * frame -- DAP_SWJ_Sequence, DAP_SWD_Sequence, DAP_SWJ_Pins, DAP_Connect -- moves the target's
 * TAP and not this one, so a host that sends one owes ID_VENDOR_CJTAG_CONTROL mode 3 again. */
uint32_t cJTAG_Tap(void);
/** Moves the tracked TAP by one clock with TMS `tms`, returning whether that clock was in a Shift
 * state, which decides that frame's shape in OScan2, OScan5 and OScan6. */
bool cJTAG_TapClock(uint32_t tms);
/** Whether the clock cJTAG_TapClock last moved was the first in a Shift state after one outside it,
 * which in OScan6 carries a PAD packet between nTDI and TDO (1149.7 rule 25.4.2 g). */
bool cJTAG_TapEntered(void);
/** Whether the tracked TAP is in Shift-DR or Shift-IR, which decides the next frame's shape. */
bool cJTAG_TapShift(void);

/** Mark the TAP as cJTAG in `format` without driving any activation, for a host that has driven
 * its own and left the TAP in `tap`, which OScan2, OScan5 and OScan6 need.
 *
 * Lowers the clock and takes the wire at the level it already holds, which is the state every
 * frame starts from. Nothing else moves, so the target sees no edge it was not sent. False, and
 * nothing changed, for a format this probe does not frame or a state that does not exist.
 */
bool cJTAG_Enter(uint32_t format, uint32_t tap);

/** Return to JTAG.
 *
 * The target stays in its cJTAG format; this only stops the probe framing for it. Getting the
 * target back is a power-on reset, so this exists for the host to undo its own mode switch, not to
 * undo the target's. In OScan2 and OScan6 one more frame goes out first, where it cannot move the TAP: the
 * last frame outside Shift advances the target's TAP only at the next TCKC period (1149.7
 * 25.6.2), so without it a host's walk into Test-Logic-Reset stops one state short.
 */
void cJTAG_Deactivate(void);

/** One OScan1 frame: the three TMSC packets that carry one JTAG clock.
 *
 * nTDI then TMS driven by us, then TDO with the wire released. Returns the TDO bit.
 *
 * Entered and left with TCKC low and the wire driven, which is what cJTAG_Enter leaves behind.
 *
 * Two copies, paced and not. The caller picks once per transfer; see the note in cjtag.c for why
 * it cannot be picked per frame.
 */
uint32_t cJTAG_FrameSlow(uint32_t tms, uint32_t tdi);
uint32_t cJTAG_FrameFast(uint32_t tms, uint32_t tdi);

/** One frame in OScan2, OScan5 or OScan6, paced, by the tracked TAP state, which it moves.
 *
 * OScan2: OScan1's three packets in a Shift state, and outside one a single TMS packet, for which
 * the target sends nothing back and this returns 1, the level a released wire reads there.
 *
 * OScan5: OScan1's three packets outside a Shift state, and in one nTDI and TDO alone, the TMS of
 * which is 0 unless the nTDI packet carries an EOT escape, two TMSC edges with TCKC high (1149.7
 * Figure 25-11). So TMS 1 in Shift, the move to Exit1, is that escape.
 *
 * OScan6: OScan2's frames outside a Shift state and OScan5's in one, with a PAD packet between nTDI
 * and TDO in the first frame after entering it. */
uint32_t cJTAG_FrameTap(uint32_t tms, uint32_t tdi);

#endif
