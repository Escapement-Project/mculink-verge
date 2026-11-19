// DAP_config.h first: DAP.h reads the DAP_SWD and DAP_JTAG settings it defines, so a sorted
// include block that puts DAP.h ahead of it will not compile.
#include "DAP_config.h"

#include "DAP.h"
#include "cjtag.h"
#include "hot.h"
#include "jtag_dp.h"

static bool active;
static uint8_t selected = CJTAG_SELECT_JTAG; // DAP_JTAG_Format's value while disconnected
static uint8_t format = CJTAG_OSCAN1;
static uint8_t tap = CJTAG_TAP_IDLE;
static bool last_shift;  // the frame before the last one clocked was in a Shift state
static bool entered;     // the last frame clocked was a Shift state's first

bool cJTAG_Active(void) { return active; }

uint32_t cJTAG_Format(void) { return format; }

uint32_t cJTAG_Take(void) {
    const uint32_t value = selected;
    selected = CJTAG_SELECT_JTAG;
    return value;
}

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
/** At least a quarter of it, never none: ARM's loop counts 0 as 2^32. The wait between a fall the
 * target samples a packet on and the next TMSC store, since a TAP.7 samples on the falling edge
 * until STC1 says otherwise (1149.7 23.4.1.4.4). After a TDO packet's fall nothing waits. Half the
 * clock delay comes short: Set_Clock_Delay takes the port write out of the half period, and ARM's
 * loop runs a cycle short on its last pass, so at 10 MHz it gave 2 cycles where a quarter is 4. */
#define CJTAG_QUARTER() PIN_DELAY_SLOW((DAP_Data.clock_delay + 3U) / 2U)

bool cJTAG_Enter(uint32_t scan_format, uint32_t tap_state) {
    if ((scan_format >= 32U) || ((CJTAG_FORMATS & (1UL << scan_format)) == 0U) || (tap_state >= CJTAG_TAP_STATES)) {
        return false;
    }
    PIN_SWCLK_TCK_CLR();
    PIN_SWDIO_OUT_ENABLE();
    format = (uint8_t)scan_format;
    tap = (uint8_t)tap_state;
    jgen_RestateTdi();
    // A host that leaves the TAP in a Shift state was clocking it there.
    last_shift = cJTAG_TapShift();
    active = true;
    return true;
}

bool cJTAG_Select(uint32_t value) {
    if ((value != CJTAG_SELECT_JTAG) && ((value >= 8U) || ((CJTAG_SELECTABLE & (1UL << value)) == 0U))) {
        return false;
    }
    if (DAP_Data.debug_port == DAP_PORT_SWD) {
        return false;
    }
    if (DAP_Data.debug_port != DAP_PORT_JTAG) {
        // Disconnected: the next DAP_Connect(2) starts in it.
        selected = (uint8_t)value;
        return true;
    }
    if (value == CJTAG_SELECT_JTAG) {
        if (active) {
            cJTAG_Deactivate();
            PORT_CJTAG_TO_JTAG();
        }
    } else {
        if (!active) {
            PORT_CJTAG_FROM_JTAG();
        }
        (void)cJTAG_Enter(8U + value, CJTAG_TAP_IDLE);
    }
    return true;
}

void cJTAG_Deactivate(void) { active = false; }

void cJTAG_Disconnect(void) {
    cJTAG_Deactivate();
    selected = CJTAG_SELECT_JTAG;
}

void cJTAG_Connect(uint32_t value) {
    PORT_CJTAG_CONNECT();
    DAP_Data.debug_port = DAP_PORT_JTAG;
    (void)cJTAG_Enter(8U + value, CJTAG_TAP_IDLE);
}

/** One TMSC packet after a fall the target may sample: the level moves a quarter period after that
 * fall, and the low phase is two of them. Entered and left with the clock low, so the level only
 * ever moves in the low half of a cycle. */
#define CJTAG_SAMPLED_PACKET(level, quarter, delay) \
    quarter;                                        \
    PIN_SWDIO_OUT(level);                           \
    quarter;                                        \
    PIN_SWCLK_TCK_SET();                            \
    delay;                                          \
    PIN_SWCLK_TCK_CLR()

/* Paced C: every constraint below is a least time, which ARM's delay loop only lengthens, and
 * tools/jtagcheck.py holds both compilers to them at every clock. Each waits a quarter period
 * before its first store too, since the fall before may be one the target samples. */

void cJTAG_Escape(uint32_t edges, uint32_t tms) {
    // At least a whole period of the clock the host set, however long: the count and the clock are
    // the host's, and with them how long TCKC stays high. A half period is up to three passes of
    // ARM's loop a clock delay and two more cycles (Set_Clock_Delay), and the loop's last pass is a
    // cycle short: two passes a clock delay came one to five cycles short with clang.
    const uint32_t gap = (2U * DAP_Data.clock_delay) + 2U;
    // Conventional JTAG rests TCK high: an escape starts from TCKC low all the same.
    PIN_SWCLK_TCK_CLR();
    uint32_t level = tms & 1U;
    CJTAG_QUARTER();
    PIN_SWDIO_OUT(level);
    PIN_SWDIO_OUT_ENABLE();
    CJTAG_QUARTER();
    PIN_SWCLK_TCK_SET();
    for (uint32_t i = 0U; i < edges; i++) {
        PIN_DELAY_SLOW(gap);
        level ^= 1U;
        PIN_SWDIO_OUT(level);
    }
    PIN_DELAY_SLOW(gap);
    PIN_SWCLK_TCK_CLR();
    CJTAG_QUARTER();
}

void cJTAG_Clock(uint32_t count, const uint8_t* bits) {
    for (uint32_t i = 0U; i < count; i++) {
        CJTAG_SAMPLED_PACKET((uint32_t)(bits[i / 8U] >> (i % 8U)) & 1U, CJTAG_QUARTER(), CJTAG_DELAY());
    }
    CJTAG_QUARTER();
}
