#include "dio.h"

#include "fsl_clock.h"
#include "fsl_device_registers.h"
#include "fsl_iocon.h"

#include "board_pins.h"
#include "dap_core.h"
#include "dap_shared.h"
#include "dio_pins.h"
#include "led.h"
#include "marker.h"

typedef struct {
    uint8_t port;
    uint8_t pin;
} dio_pin_t;

/** DIO0 to DIO3, each a GPIO on function 0: the Pro's (src/dio_pins.h), and J8's pins 1 to 4. */
static const dio_pin_t pro_pins[4] = {{0U, 20U}, {0U, 26U}, {1U, 2U}, {1U, 3U}};
static const dio_pin_t j8_pins[4] = {{1U, 0U}, {1U, 1U}, {1U, 2U}, {1U, 3U}};

/** The Pro's IO_DIR1 and IO_DIR2, high to drive the header, and the pins each turns. */
static const dio_pin_t dio_dir[2] = {{1U, 1U}, {1U, 25U}};
static const uint8_t pro_groups[2] = {0x07U, 0x08U};
/** J8 has no translators, so each pin turns on its own. */
static const uint8_t j8_groups[4] = {0x01U, 0x02U, 0x04U, 0x08U};

/** The Pro's pins have 1K pull-downs on the board; J8's have only the MCU's. */
#define DIO_PIN_MODE (IOCON_FUNC0 | IOCON_MODE_INACT | IOCON_DIGITAL_EN)
#define J8_PIN_MODE (IOCON_FUNC0 | IOCON_MODE_PULLDOWN | IOCON_DIGITAL_EN)

static dio_pin_t const* dio_pins = pro_pins;
static uint8_t const* dio_group = pro_groups;
static uint8_t dio_groups;
/** The Pro: a direction pin turns each group's translators. */
static bool dio_translated;
/** The board has the pins and its jumpers leave them on: what the capability bits report. */
static bool dio_present;
/** The pins a host can drive: all of them, less any the bench marker holds. */
static uint8_t dio_on;
static uint8_t dio_outputs;
static uint8_t dio_levels;
static bool dio_spi_owned;

/** One store per port of `regs` (SET, CLR, DIRSET or DIRCLR), for the DIO pins in `pins`. */
static void dio_store(volatile uint32_t* regs, uint32_t pins) {
    uint32_t words[2] = {0U, 0U};
    for (uint32_t n = 0U; n < 4U; n++) {
        if ((pins & (1UL << n)) != 0U) {
            words[dio_pins[n].port] |= 1UL << dio_pins[n].pin;
        }
    }
    for (uint32_t port = 0U; port < 2U; port++) {
        if (words[port] != 0U) {
            regs[port] = words[port];
        }
    }
}

/** A group's direction pin, on the Pro: high turns its translators to drive the header. */
static void dio_turn(uint32_t group, bool out) {
    if (!dio_translated) {
        return;
    }
    volatile uint32_t* regs = out ? GPIO->SET : GPIO->CLR;
    regs[dio_dir[group].port] = 1UL << dio_dir[group].pin;
}

/** The pins' levels now, bit n DIOn. */
static uint8_t dio_read(void) {
    uint32_t const ports[2] = {GPIO->PIN[0], GPIO->PIN[1]};
    uint32_t read = 0U;
    for (uint32_t n = 0U; n < 4U; n++) {
        read |= ((ports[dio_pins[n].port] >> dio_pins[n].pin) & 1U) << n;
    }
    return (uint8_t)read;
}

/** J8's pins as inputs, pulled down: nothing else on the plain board touches them. */
static void j8_init(void) {
    CLOCK_EnableClock(kCLOCK_Gpio1);
    dio_pins = j8_pins;
    dio_group = j8_groups;
    dio_groups = 4U;
    for (uint32_t n = 0U; n < 4U; n++) {
        IOCON_PinMuxSet(IOCON, j8_pins[n].port, j8_pins[n].pin, J8_PIN_MODE);
    }
    dio_store(GPIO->DIRCLR, DIO_PINS);
    dio_present = true;
    dio_on = DIO_PINS;
}

__attribute__((noinline)) void dio_init(void) {
    if (board_kind == BOARD_KIND_PLAIN) {
        j8_init();
        return;
    }
    if ((board_kind != BOARD_KIND_PRO) || ((dap_shared.jumpers & BOARD_JUMPER_USBSIO_OFF) != 0U)) {
        return;
    }
    dio_translated = true;
    dio_present = true;
    dio_groups = 2U;
    CLOCK_EnableClock(kCLOCK_Gpio0);
    CLOCK_EnableClock(kCLOCK_Gpio1);
    // The pull-downs already hold both directions toward the probe; driving them low keeps it so.
    for (uint32_t group = 0U; group < 2U; group++) {
        GPIO->CLR[dio_dir[group].port] = 1UL << dio_dir[group].pin;
        GPIO->DIRSET[dio_dir[group].port] = 1UL << dio_dir[group].pin;
        IOCON_PinMuxSet(IOCON, dio_dir[group].port, dio_dir[group].pin, DIO_PIN_MODE);
    }
    dio_store(GPIO->DIRCLR, DIO_PINS);
    for (uint32_t n = 0U; n < 4U; n++) {
        IOCON_PinMuxSet(IOCON, dio_pins[n].port, dio_pins[n].pin, DIO_PIN_MODE);
    }
    dio_on = DIO_PINS;
}

bool dio_available(void) {
    return dio_present;
}

bool dio_set(uint8_t mask, uint8_t outputs, uint8_t levels) {
    if (!dio_present || dio_spi_owned || ((mask & ~dio_on) != 0U)) {
        return false;
    }
    const uint32_t next = (dio_outputs & ~(uint32_t)mask) | (outputs & (uint32_t)mask);
    for (uint32_t group = 0U; group < dio_groups; group++) {
        const uint32_t turned = next & dio_group[group];
        if ((turned != 0U) && (turned != dio_group[group])) {
            return false;
        }
    }

    // Groups turning to input let go at the level they drive, then every latch is written, so a
    // pin turning to output comes up at its level and none moves twice.
    //
    // The probe's pin and the translator never drive the line between them at once: the translator
    // turns toward the probe only after the probe lets go, and away from it before the probe
    // drives. Between the two stores nothing drives the translator's input, which holds its last
    // level on its own charge, so the header moves once. Masked, so nothing stretches that window.
    const uint32_t primask = __get_PRIMASK();
    __disable_irq();
    for (uint32_t group = 0U; group < dio_groups; group++) {
        if (((dio_outputs & dio_group[group]) != 0U) && ((next & dio_group[group]) == 0U)) {
            dio_store(GPIO->DIRCLR, dio_group[group]);
            dio_turn(group, false);
        }
    }
    dio_levels = (uint8_t)((dio_levels & ~mask) | (levels & mask));
    dio_store(GPIO->SET, levels & mask);
    dio_store(GPIO->CLR, ~levels & mask);
    for (uint32_t group = 0U; group < dio_groups; group++) {
        if (((dio_outputs & dio_group[group]) == 0U) && ((next & dio_group[group]) != 0U)) {
            dio_turn(group, true);
            dio_store(GPIO->DIRSET, dio_group[group]);
        }
    }
    __set_PRIMASK(primask);

    dio_outputs = (uint8_t)next;
    led_sio(next);
    return true;
}

void dio_get(dio_state_t* state) {
    *state = (dio_state_t){0};
    if (!dio_present) {
        return;
    }
    state->pins = dio_on;
    state->outputs = dio_outputs;
    state->levels = dio_levels;
    state->read = dio_read();
    state->groups = dio_groups;
    for (uint32_t group = 0U; group < dio_groups; group++) {
        state->group[group] = dio_group[group];
    }
}

bool dio_marker(uint8_t pins) {
#ifdef MARKER_PINS
    // The marker borrows J8 from DIO, which dio_init gives it on the plain board alone.
    if (dio_pins != j8_pins) {
        return false;
    }
    uint32_t const want = pins & J8_PIN_MASK;
    uint32_t const taken = want & ~marker_mask;
    uint32_t const given = marker_mask & ~want;
    // This command runs with the marker's pins high. A pin it takes stops being a DIO pin and
    // drives low until the next command; one it gives back goes low, then becomes an input again.
    J8_GPIO->CLR[J8_PORT] = taken | given;
    J8_GPIO->DIRSET[J8_PORT] = taken;
    J8_GPIO->DIRCLR[J8_PORT] = given;
    marker_mask = want;
    dio_on = (uint8_t)(DIO_PINS & ~want);
    dio_outputs = (uint8_t)(dio_outputs & ~want);
    dio_levels = (uint8_t)(dio_levels & ~want);
    led_sio(dio_outputs);
    return true;
#else
    (void)pins;
    return false;
#endif
}

uint8_t dio_stream_pins(uint8_t requested, uint8_t* levels) {
    uint8_t const pins = (dio_spi_owned || !dio_translated) ? 0U : (uint8_t)(requested & dio_on);
    *levels = (uint8_t)(dio_pins_levels(GPIO->PIN[0], GPIO->PIN[1]) & pins);
    return pins;
}

__attribute__((noinline)) bool dio_spi_begin(void) {
    if (!dio_translated || (dio_on == 0U) || (dio_outputs != 0U) || dio_spi_owned) {
        return false;
    }

    // A stream reports no edges while SPI has the pins: stop the pin interrupts before SPI moves
    // them. CPU 0 turns them back on afterwards (src/dio_edges.c), and its stream marks the edges
    // lost from this stamp.
    // A transfer CPU 0 hasn't seen yet left the pins off from its own start, which its stamp holds.
    // CPU 0 starts a loss after the last edge it sent whatever the stamp (src/energy_stream.c), so
    // one kept as CPU 0 catches up is safe. CTIMER4 runs from the first energy capture on, and the
    // stamp matters only to a stream, which needs one; before that its clock is off.
    if (dap_shared.dio_spi_seen == dap_shared.dio_spi_turns) {
        bool const stamping = (SYSCON->AHBCLKCTRL.AHBCLKCTRL2 & SYSCON_AHBCLKCTRL2_TIMER4_MASK) != 0U;
        dap_shared.dio_spi_stamp = stamping ? CTIMER4->TC : 0U;
    }
    __DMB();
    dap_shared.dio_spi_turns++;
    __DMB();
    PINT->CIENR = DIO_PINS;
    PINT->CIENF = DIO_PINS;

    // Keep the probe isolated while the peripheral takes control of its output pins.
    IOCON_PinMuxSet(IOCON, 0U, 20U, DIO_PIN_MODE);
    IOCON_PinMuxSet(IOCON, 0U, 26U, DIO_PIN_MODE);
    IOCON_PinMuxSet(IOCON, 1U, 2U, DIO_PIN_MODE);
    IOCON_PinMuxSet(IOCON, 1U, 3U, DIO_PIN_MODE);
    dio_turn(0U, true);
    dio_turn(1U, false);
    dio_spi_owned = true;
    led_sio(true);
    return true;
}

__attribute__((noinline)) void dio_spi_end(void) {
    if (!dio_spi_owned) {
        return;
    }

    // Turn the output translators away before restoring GPIO input mode.
    dio_turn(0U, false);
    IOCON_PinMuxSet(IOCON, 0U, 20U, DIO_PIN_MODE);
    IOCON_PinMuxSet(IOCON, 0U, 26U, DIO_PIN_MODE);
    IOCON_PinMuxSet(IOCON, 1U, 2U, DIO_PIN_MODE);
    IOCON_PinMuxSet(IOCON, 1U, 3U, DIO_PIN_MODE);
    dio_store(GPIO->DIRCLR, DIO_PINS);
    dio_spi_owned = false;
    led_sio(dio_outputs != 0U);
    __DMB();
    dap_shared.dio_spi_turns++;
    __DMB();
    MAILBOX->MBOXIRQ[DAP_CORE_MAILBOX_CPU0].IRQSET = DAP_CORE_MAILBOX_WAKE_MASK;
}
