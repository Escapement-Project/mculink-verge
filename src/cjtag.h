/** IEEE 1149.7 (cJTAG) support, OScan1, OScan2, OScan5 and OScan6 formats.
 *
 * The interface is ARM's proposed DAP_JTAG_Format and DAP_JTAG_Escape (CMSIS-DAP#8, revision 0.1)
 * with Verge's extensions, at vendor IDs until ARM assigns them. The host connects with
 * DAP_Connect(2), selects a format on that connection, and drives the target's activation itself:
 * escapes, and raw TMSC bits through DAP_SWJ_Sequence, which with a format selected changes TMSC only
 * while TCKC is low. A probe cannot tell from the wire whether a target speaks JTAG or cJTAG, so
 * nothing here guesses.
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

/** DAP_JTAG_Format's values: 0 conventional JTAG, n for OScan n, which is SCNFMT 8 + n; bit n of
 * this is format n taken. A host finds them by trying each on a JTAG connection. */
#define CJTAG_SELECT_JTAG 0U
#define CJTAG_SELECTABLE ((1UL << 1) | (1UL << 2) | (1UL << 5) | (1UL << 6))

/** DAP_JTAG_Format: the format of the live JTAG connection, conventional JTAG or an OScan format,
 * at once. Every connection starts conventional, so nothing outlives it. It sends no TCKC period:
 * into an OScan format TCKC goes low and TDI is let go, and framing starts from Run-Test/Idle; back
 * to conventional TDI is driven again. In OScan2 and OScan6 the target's last move outside Shift
 * completes at the next TCKC rise (1149.7 25.6.1), so the host, which knows the TAP state, clocks one
 * more frame first. A change from one OScan format to another sends nothing either: the host's STFMT
 * and check packet have already moved the target. False, and nothing changed, with no JTAG
 * connection or for a value not offered. */
bool cJTAG_Select(uint32_t value);

/** DAP_JTAG_Escape: one TCKC period with `edges` TMSC transitions while TCKC is high. TMSC goes to
 * `level` a quarter period after the last fall and a quarter period before the rise; an odd count
 * leaves it at the other level. The rise, each edge and the fall come one TCKC period after the one
 * before (1149.7 10.4.2 b), at whatever clock the host set, then a quarter period's wait, since the
 * target counts the period as a bit. Entered and left with TCKC low; framing is untouched. */
void cJTAG_Escape(uint32_t edges, uint32_t level);
/** DAP_SWJ_Sequence with an OScan format selected: `count` raw TCKC periods, TMSC from `bits` least
 * significant bit first, each change a quarter period after the fall before, then a quarter period
 * after the last fall. Moves no tracked state: the target takes these as raw packets. */
void cJTAG_Clock(uint32_t count, const uint8_t* bits);

/** TAP controller states, in 1149.1's order. */
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

/** Whether the TAP is being driven over cJTAG rather than JTAG: an OScan format selected and the
 * probe connected over JTAG. */
bool cJTAG_Active(void);

/** The scan format cJTAG frames take, CJTAG_OSCAN1, _OSCAN2, _OSCAN5 or _OSCAN6. */
uint32_t cJTAG_Format(void);

/** OScan2's, OScan5's and OScan6's frames depend on the TAP state the frame is clocked in, so the probe
 * follows the TAP from the TMS it sends. Every frame in those formats has to go through
 * cJTAG_TapClock, or the next frame takes the wrong shape and the target loses the frame
 * boundaries. A clock the probe does not
 * frame -- DAP_SWD_Sequence, DAP_SWJ_Pins, raw DAP_SWJ_Sequence -- moves the target's TAP and not
 * this one, so a host that sends one restates Run-Test/Idle with DAP_JTAG_Format again. */
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

/** Stop framing: DAP_JTAG_Format 0, and any DAP_Connect the probe accepts or DAP_Disconnect. It sends
 * no TCKC period, but the pin set-up after it may raise TCKC once: ARM's SWD and JTAG set-up leave it
 * high, and after DAP_Disconnect it rests on the target's pull-up.
 *
 * The target stays in its cJTAG format; this only stops the probe framing for it. In OScan2 and
 * OScan6 the last frame outside Shift advances the target's TAP at the next TCKC rise (1149.7
 * 25.6.1), whatever drives it, so a host clocks one more frame first to land it on a rise it chose.
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
