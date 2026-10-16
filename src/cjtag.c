// DAP_config.h first: DAP.h reads the DAP_SWD and DAP_JTAG settings it defines, so a sorted
// include block that puts DAP.h ahead of it will not compile.
#include "DAP_config.h"

#include "DAP.h"
#include "cjtag.h"

static bool active;

bool cJTAG_Active(void) { return active; }

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
    active = true;
}

void cJTAG_Enter(void) {
    PIN_SWCLK_TCK_CLR();
    PIN_SWDIO_OUT_ENABLE();
    active = true;
}

void cJTAG_Deactivate(void) { active = false; }

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
