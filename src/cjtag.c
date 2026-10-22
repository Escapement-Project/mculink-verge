// DAP_config.h first: DAP.h reads the DAP_SWD and DAP_JTAG settings it defines, so a sorted
// include block that puts DAP.h ahead of it will not compile.
#include "DAP_config.h"

#include "DAP.h"
#include "cjtag.h"
#include "hot.h"

static bool active;
static uint8_t format = CJTAG_OSCAN1;
static uint8_t tap = CJTAG_TAP_IDLE;
static bool last_shift;  // the frame before the last one clocked was in a Shift state
static bool entered;     // the last frame clocked was a Shift state's first
static uint32_t offered = CJTAG_FORMATS;

bool cJTAG_Active(void) { return active; }

uint32_t cJTAG_Format(void) { return format; }

uint32_t cJTAG_Offered(void) { return offered; }

void cJTAG_Offer(uint32_t mask) { offered = (mask & CJTAG_FORMATS) | (1UL << CJTAG_OSCAN1); }

COMMAND_PATH uint32_t cJTAG_Tap(void) { return tap; }

/** IEEE 1149.1's state diagram: the next state for TMS 0 and for TMS 1. */
static const uint8_t tap_next[CJTAG_TAP_STATES][2] = {
    [CJTAG_TAP_RESET] = {CJTAG_TAP_IDLE, CJTAG_TAP_RESET},
    [CJTAG_TAP_IDLE] = {CJTAG_TAP_IDLE, CJTAG_TAP_SELECT_DR},
    [CJTAG_TAP_SELECT_DR] = {CJTAG_TAP_CAPTURE_DR, CJTAG_TAP_SELECT_IR},
    [CJTAG_TAP_CAPTURE_DR] = {CJTAG_TAP_SHIFT_DR, CJTAG_TAP_EXIT1_DR},
    [CJTAG_TAP_SHIFT_DR] = {CJTAG_TAP_SHIFT_DR, CJTAG_TAP_EXIT1_DR},
    [CJTAG_TAP_EXIT1_DR] = {CJTAG_TAP_PAUSE_DR, CJTAG_TAP_UPDATE_DR},
    [CJTAG_TAP_PAUSE_DR] = {CJTAG_TAP_PAUSE_DR, CJTAG_TAP_EXIT2_DR},
    [CJTAG_TAP_EXIT2_DR] = {CJTAG_TAP_SHIFT_DR, CJTAG_TAP_UPDATE_DR},
    [CJTAG_TAP_UPDATE_DR] = {CJTAG_TAP_IDLE, CJTAG_TAP_SELECT_DR},
    [CJTAG_TAP_SELECT_IR] = {CJTAG_TAP_CAPTURE_IR, CJTAG_TAP_RESET},
    [CJTAG_TAP_CAPTURE_IR] = {CJTAG_TAP_SHIFT_IR, CJTAG_TAP_EXIT1_IR},
    [CJTAG_TAP_SHIFT_IR] = {CJTAG_TAP_SHIFT_IR, CJTAG_TAP_EXIT1_IR},
    [CJTAG_TAP_EXIT1_IR] = {CJTAG_TAP_PAUSE_IR, CJTAG_TAP_UPDATE_IR},
    [CJTAG_TAP_PAUSE_IR] = {CJTAG_TAP_PAUSE_IR, CJTAG_TAP_EXIT2_IR},
    [CJTAG_TAP_EXIT2_IR] = {CJTAG_TAP_SHIFT_IR, CJTAG_TAP_UPDATE_IR},
    [CJTAG_TAP_UPDATE_IR] = {CJTAG_TAP_IDLE, CJTAG_TAP_SELECT_DR},
};

COMMAND_PATH bool cJTAG_TapShift(void) { return (tap == CJTAG_TAP_SHIFT_DR) || (tap == CJTAG_TAP_SHIFT_IR); }

COMMAND_PATH bool cJTAG_TapClock(uint32_t tms) {
    const bool shift = cJTAG_TapShift();
    entered = shift && !last_shift;
    last_shift = shift;
    tap = tap_next[tap][tms & 1U];
    return shift;
}

COMMAND_PATH bool cJTAG_TapEntered(void) { return entered; }

/** One half-period of whatever clock the host asked for. */
#define CJTAG_DELAY() PIN_DELAY_SLOW(DAP_Data.clock_delay)

/* The activation table that stood here was transcribed from OpenOCD's ftdi.c, which is
 * GPL-2.0-or-later, so it is left out of the published history. The probe stopped driving an
 * activation of its own when it was licensed under Apache-2.0. */

void cJTAG_Activate(void) {
    PIN_SWDIO_OUT_ENABLE();

    // OpenOCD sets both pins of a row in one write. Here they are two, so the order decides what
    // the target sees: TMSC moves after a falling edge and before a rising one. Only the escape
    // rows, where the clock is already high and stays high, move it with the clock high.
    for (uint32_t i = 0U; i < sizeof(activation) / sizeof(activation[0]); i++) {
        if (activation[i].tck != 0U) {
            PIN_SWDIO_OUT(activation[i].tmsc);
            PIN_SWCLK_TCK_SET();
        } else {
            PIN_SWCLK_TCK_CLR();
            PIN_SWDIO_OUT(activation[i].tmsc);
        }
        CJTAG_DELAY();
    }

    // Leave the clock low and the wire ours, which is the state every frame below starts from.
    PIN_SWCLK_TCK_CLR();
    format = CJTAG_OSCAN1;
    active = true;
}

bool cJTAG_Enter(uint32_t scan_format, uint32_t tap_state) {
    if ((scan_format >= 32U) || ((offered & (1UL << scan_format)) == 0U) || (tap_state >= CJTAG_TAP_STATES)) {
        return false;
    }
    PIN_SWCLK_TCK_CLR();
    PIN_SWDIO_OUT_ENABLE();
    format = (uint8_t)scan_format;
    tap = (uint8_t)tap_state;
    // A host that leaves the TAP in a Shift state was clocking it there.
    last_shift = cJTAG_TapShift();
    active = true;
    return true;
}

void cJTAG_Deactivate(void) {
    // After DAP_Disconnect the pins are the target's again, and a frame would take them back.
    // OScan5's frames all carry TDO, so its TAP has advanced by the end of each.
    if (active && ((format == CJTAG_OSCAN2) || (format == CJTAG_OSCAN6)) && (DAP_Data.debug_port == DAP_PORT_JTAG)) {
        if (tap == CJTAG_TAP_RESET) {
            (void)cJTAG_FrameTap(1U, 1U);
        } else if (tap == CJTAG_TAP_IDLE) {
            (void)cJTAG_FrameTap(0U, 1U);
        }
    }
    active = false;
}

/** Drive one TMSC packet: set the level while the clock is low, then clock it in.
 *
 * Entered and left with the clock low, so the level only ever moves in the low half of a cycle.
 */
#define CJTAG_PACKET(level, delay) \
    PIN_SWDIO_OUT(level);          \
    delay;                         \
    PIN_SWCLK_TCK_SET();           \
    delay;                         \
    PIN_SWCLK_TCK_CLR()

/** One OScan1 frame: the three TMSC packets that carry one JTAG clock.
 *
 * Packet 1 carries the inverse of TDI, which is what the format specifies. Packet 2 carries TMS.
 * Packet 3 is the target's: release the wire -- both the pin and the buffer that drives it, or the
 * buffer keeps driving whatever the pin does -- and sample what comes back.
 *
 * Taking the wire back afterwards is a TMSC move like any other: it jumps from whatever the target
 * drove to whatever our output latch holds. So it happens after the falling edge, not before it.
 * Doing it while TCKC was still high put one escape edge in every frame, which is the exact fault
 * this path exists to avoid. OpenOCD's OScan1 driver sequences it the same way, with TCK resting
 * low between frames.
 *
 * Two copies, paced and not, for the reason ARM splits each of its own transfers the same way:
 * above the switch point the pacing loop is longer than the clock the host asked for. The choice
 * is made once per transfer rather than once per frame -- putting the branch inside the frame
 * costs a cycle in the paced copy, which is enough to take a 10 MHz request over the line.
 */
#define CJTAG_FRAME_FUNCTION(speed, delay)                    \
    uint32_t cJTAG_Frame##speed(uint32_t tms, uint32_t tdi) { \
        uint32_t tdo;                                         \
                                                              \
        CJTAG_PACKET(tdi ^ 1U, delay);                        \
        CJTAG_PACKET(tms, delay);                             \
                                                              \
        PIN_SWDIO_OUT_DISABLE();                              \
        delay;                                                \
        tdo = PIN_SWDIO_IN();                                 \
        PIN_SWCLK_TCK_SET();                                  \
        delay;                                                \
        PIN_SWCLK_TCK_CLR();                                  \
        PIN_SWDIO_OUT_ENABLE();                               \
                                                              \
        return tdo;                                           \
    }

CJTAG_FRAME_FUNCTION(Slow, CJTAG_DELAY())
CJTAG_FRAME_FUNCTION(Fast, PIN_DELAY_FAST())

uint32_t cJTAG_FrameTap(uint32_t tms, uint32_t tdi) {
    const bool shift = cJTAG_TapClock(tms);
    if (((format == CJTAG_OSCAN2) || (format == CJTAG_OSCAN6)) && !shift) {
        CJTAG_PACKET(tms, CJTAG_DELAY());
        return 1U;
    }
    if (((format == CJTAG_OSCAN5) || (format == CJTAG_OSCAN6)) && shift) {
        PIN_SWDIO_OUT(tdi ^ 1U);
        CJTAG_DELAY();
        PIN_SWCLK_TCK_SET();
        CJTAG_DELAY();
        if (tms != 0U) {
            // Each escape edge, and the fall after the last, a whole period after the one before:
            // the standard's minimum is the target's shortest TCKC period, and the host's clock is
            // no shorter. Three half-period delays, not two: from 10 MHz up a delay is shorter
            // than the half period it is for, which a packet's stores make up and a gap does not.
            CJTAG_DELAY();
            CJTAG_DELAY();
            PIN_SWDIO_OUT(tdi);
            CJTAG_DELAY();
            CJTAG_DELAY();
            CJTAG_DELAY();
            PIN_SWDIO_OUT(tdi ^ 1U);
            CJTAG_DELAY();
            CJTAG_DELAY();
            CJTAG_DELAY();
        }
        PIN_SWCLK_TCK_CLR();
        if ((format == CJTAG_OSCAN6) && entered) {
            // The PAD packet, at nTDI's level, so that it moves nothing.
            CJTAG_PACKET(tdi ^ 1U, CJTAG_DELAY());
        }
    } else {
        CJTAG_PACKET(tdi ^ 1U, CJTAG_DELAY());
        CJTAG_PACKET(tms, CJTAG_DELAY());
    }

    PIN_SWDIO_OUT_DISABLE();
    CJTAG_DELAY();
    const uint32_t tdo = PIN_SWDIO_IN();
    PIN_SWCLK_TCK_SET();
    CJTAG_DELAY();
    PIN_SWCLK_TCK_CLR();
    PIN_SWDIO_OUT_ENABLE();
    return tdo;
}
