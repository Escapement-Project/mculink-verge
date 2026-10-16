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

    for (uint32_t i = 0U; i < sizeof(activation) / sizeof(activation[0]); i++) {
        PIN_SWDIO_OUT(activation[i].tmsc);
        if (activation[i].tck != 0U) {
            PIN_SWCLK_TCK_SET();
        } else {
            PIN_SWCLK_TCK_CLR();
        }
        CJTAG_DELAY();
    }

    PIN_SWCLK_TCK_CLR();
    active = true;
}

void cJTAG_Deactivate(void) { active = false; }

/** Drive one TMSC packet: set the level while the clock is low, then clock it in.
 *
 * The ordering is the entire fix. A level change while TCKC is high is an escape edge to a 1149.7
 * target, so data must only move in the low half of the cycle.
 */
__STATIC_FORCEINLINE void cjtag_packet_out(uint32_t level) {
    PIN_SWCLK_TCK_CLR();
    PIN_SWDIO_OUT(level);
    CJTAG_DELAY();
    PIN_SWCLK_TCK_SET();
    CJTAG_DELAY();
}

uint32_t cJTAG_Frame(uint32_t tms, uint32_t tdi) {
    // Packet 1 carries the inverse of TDI, which is what the format specifies.
    cjtag_packet_out(tdi ^ 1U);

    // Packet 2 carries TMS.
    cjtag_packet_out(tms);

    // Packet 3 is the target's. Release the wire -- both the pin and the buffer that drives it, or
    // the buffer keeps driving whatever the pin does -- and sample what comes back.
    PIN_SWCLK_TCK_CLR();
    PIN_SWDIO_OUT_DISABLE();
    CJTAG_DELAY();
    uint32_t tdo = PIN_SWDIO_IN();
    PIN_SWCLK_TCK_SET();
    CJTAG_DELAY();
    PIN_SWDIO_OUT_ENABLE();

    return tdo;
}
