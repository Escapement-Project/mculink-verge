// DAP_config.h first: DAP.h reads the DAP_SWD and DAP_JTAG settings it defines, so a sorted
// include block that puts DAP.h ahead of it will not compile.
#include "DAP_config.h"

#include "DAP.h"
#include "cjtag.h"
#include "hot.h"

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
/** The same for the delay-free frame, which runs as fast as its stores. */
#define CJTAG_QUARTER_FAST() \
    __NOP();                 \
    __NOP()

bool cJTAG_Enter(uint32_t scan_format, uint32_t tap_state) {
    if ((scan_format >= 32U) || ((CJTAG_FORMATS & (1UL << scan_format)) == 0U) || (tap_state >= CJTAG_TAP_STATES)) {
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

/** The same after a packet the target samples: the level moves a quarter period after that fall,
 * and the low phase is two of them. */
#define CJTAG_SAMPLED_PACKET(level, quarter, delay) \
    quarter;                                        \
    PIN_SWDIO_OUT(level);                           \
    quarter;                                        \
    PIN_SWCLK_TCK_SET();                            \
    delay;                                          \
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
#ifdef CJTAG_ZERO_HOLD
/** Lab only (CMake CJTAG_ZERO_HOLD, version "+zero-hold-lab"): the fall that ends the nTDI packet
 * and the TMS packet's level go out in one store through the masked port, so a target sampling on
 * the falling edge gets no hold at all. It measures whether the quarter period is needed; a real
 * build never moves TMSC in the store of a TCKC edge. */
__attribute__((noinline)) static void cjtag_fall_with(uint32_t level) {
    const uint32_t mask = DBGIF_TCK_SWCLK_GPIO->MASK[DBGIF_TCK_SWCLK_PORT];
    DBGIF_TCK_SWCLK_GPIO->MASK[DBGIF_TCK_SWCLK_PORT] =
        ~((1U << DBGIF_TCK_SWCLK_PIN) | (1U << DBGIF_TMS_SWDIO_PIN));
    DBGIF_TCK_SWCLK_GPIO->MPIN[DBGIF_TCK_SWCLK_PORT] = (level & 1U) << DBGIF_TMS_SWDIO_PIN;
    DBGIF_TCK_SWCLK_GPIO->MASK[DBGIF_TCK_SWCLK_PORT] = mask;
}
static_assert(DBGIF_TMS_SWDIO_PORT == DBGIF_TCK_SWCLK_PORT, "one masked store moves TCKC and TMSC together");
#define CJTAG_NTDI_THEN_TMS(ntdi, tms, quarter, delay) \
    PIN_SWDIO_OUT(ntdi);                               \
    delay;                                             \
    PIN_SWCLK_TCK_SET();                               \
    delay;                                             \
    cjtag_fall_with(tms);                              \
    delay;                                             \
    PIN_SWCLK_TCK_SET();                               \
    delay;                                             \
    PIN_SWCLK_TCK_CLR()
#elif defined(CJTAG_ZERO_SETUP)
/** Lab only (CMake CJTAG_ZERO_SETUP, version "+zero-setup-lab"), the mirror of CJTAG_ZERO_HOLD: the
 * TMS packet's level and the rise that clocks it go out in one store through the masked port, so a
 * target sampling on the rising edge gets no setup, and one sampling on the falling edge a whole high
 * phase. It tells the two sampling edges apart on a target that cannot report SREDGE. */
__attribute__((noinline)) static void cjtag_rise_with(uint32_t level) {
    const uint32_t mask = DBGIF_TCK_SWCLK_GPIO->MASK[DBGIF_TCK_SWCLK_PORT];
    DBGIF_TCK_SWCLK_GPIO->MASK[DBGIF_TCK_SWCLK_PORT] =
        ~((1U << DBGIF_TCK_SWCLK_PIN) | (1U << DBGIF_TMS_SWDIO_PIN));
    DBGIF_TCK_SWCLK_GPIO->MPIN[DBGIF_TCK_SWCLK_PORT] =
        (1U << DBGIF_TCK_SWCLK_PIN) | ((level & 1U) << DBGIF_TMS_SWDIO_PIN);
    DBGIF_TCK_SWCLK_GPIO->MASK[DBGIF_TCK_SWCLK_PORT] = mask;
}
static_assert(DBGIF_TMS_SWDIO_PORT == DBGIF_TCK_SWCLK_PORT, "one masked store moves TCKC and TMSC together");
#define CJTAG_NTDI_THEN_TMS(ntdi, tms, quarter, delay) \
    CJTAG_PACKET(ntdi, delay);                         \
    quarter;                                           \
    quarter;                                           \
    cjtag_rise_with(tms);                              \
    delay;                                             \
    PIN_SWCLK_TCK_CLR()
#else
#define CJTAG_NTDI_THEN_TMS(ntdi, tms, quarter, delay) \
    CJTAG_PACKET(ntdi, delay);                         \
    CJTAG_SAMPLED_PACKET(tms, quarter, delay)
#endif

#define CJTAG_FRAME_FUNCTION(speed, delay, quarter)           \
    uint32_t cJTAG_Frame##speed(uint32_t tms, uint32_t tdi) { \
        uint32_t tdo;                                         \
                                                              \
        CJTAG_NTDI_THEN_TMS(tdi ^ 1U, tms, quarter, delay);   \
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

CJTAG_FRAME_FUNCTION(Slow, CJTAG_DELAY(), CJTAG_QUARTER())
CJTAG_FRAME_FUNCTION(Fast, PIN_DELAY_FAST(), CJTAG_QUARTER_FAST())

uint32_t cJTAG_FrameTap(uint32_t tms, uint32_t tdi) {
    const bool shift = cJTAG_TapClock(tms);
    if (((format == CJTAG_OSCAN2) || (format == CJTAG_OSCAN6)) && !shift) {
        CJTAG_SAMPLED_PACKET(tms, CJTAG_QUARTER(), CJTAG_DELAY());
        return 1U;
    }
    // The last frame may have been OScan2's single TMS packet, which the target sampled on its
    // fall; this paced copy waits whatever came before.
    CJTAG_QUARTER();
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
        CJTAG_SAMPLED_PACKET(tms, CJTAG_QUARTER(), CJTAG_DELAY());
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

/* Paced C, as cJTAG_FrameTap is: every constraint below is a least time, which ARM's delay loop only
 * lengthens, and tools/jtagcheck.py holds both compilers to them at every clock. Each waits a quarter
 * period before its first store too, since the fall before may be one the target samples. */

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
