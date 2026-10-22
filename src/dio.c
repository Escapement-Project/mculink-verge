#include "dio.h"

#include "fsl_clock.h"
#include "fsl_device_registers.h"
#include "fsl_iocon.h"

#include "board_pins.h"
#include "dap_shared.h"
#include "led.h"

typedef struct {
    uint8_t port;
    uint8_t pin;
} dio_pin_t;

/** DIO0 to DIO3: the HS SPI's SSEL0, MOSI, SCK and MISO pins, each a GPIO on function 0. */
static const dio_pin_t dio_pins[] = {{0U, 20U}, {0U, 26U}, {1U, 2U}, {1U, 3U}};
#define DIO_PINS 0x0FU

/** IO_DIR1 and IO_DIR2, high to drive the header, and the pins each turns. */
static const dio_pin_t dio_dir[DIO_GROUPS_MAX] = {{1U, 1U}, {1U, 25U}};
static const uint8_t dio_group[DIO_GROUPS_MAX] = {0x07U, 0x08U};

#define DIO_PIN_MODE (IOCON_FUNC0 | IOCON_MODE_INACT | IOCON_DIGITAL_EN)

static uint8_t dio_on;
static uint8_t dio_outputs;
static uint8_t dio_levels;

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

/** A group's direction pin: high turns its translators to drive the header. */
static void dio_turn(uint32_t group, bool out) {
    volatile uint32_t* regs = out ? GPIO->SET : GPIO->CLR;
    regs[dio_dir[group].port] = 1UL << dio_dir[group].pin;
}

__attribute__((noinline)) void dio_init(void) {
    if ((board_kind != BOARD_KIND_PRO) || ((dap_shared.jumpers & BOARD_JUMPER_USBSIO_OFF) != 0U)) {
        return;
    }
    CLOCK_EnableClock(kCLOCK_Gpio0);
    CLOCK_EnableClock(kCLOCK_Gpio1);
    // The pull-downs already hold both directions toward the probe; driving them low keeps it so.
    for (uint32_t group = 0U; group < DIO_GROUPS_MAX; group++) {
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
    return dio_on != 0U;
}

bool dio_set(uint8_t mask, uint8_t outputs, uint8_t levels) {
    if ((dio_on == 0U) || ((mask & ~dio_on) != 0U)) {
        return false;
    }
    const uint32_t next = (dio_outputs & ~(uint32_t)mask) | (outputs & (uint32_t)mask);
    for (uint32_t group = 0U; group < DIO_GROUPS_MAX; group++) {
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
    for (uint32_t group = 0U; group < DIO_GROUPS_MAX; group++) {
        if (((dio_outputs & dio_group[group]) != 0U) && ((next & dio_group[group]) == 0U)) {
            dio_store(GPIO->DIRCLR, dio_group[group]);
            dio_turn(group, false);
        }
    }
    dio_levels = (uint8_t)((dio_levels & ~mask) | (levels & mask));
    dio_store(GPIO->SET, levels & mask);
    dio_store(GPIO->CLR, ~levels & mask);
    for (uint32_t group = 0U; group < DIO_GROUPS_MAX; group++) {
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
    if (dio_on == 0U) {
        return;
    }
    const uint32_t port[2] = {GPIO->PIN[0], GPIO->PIN[1]};
    uint8_t read = 0U;
    for (uint32_t n = 0U; n < 4U; n++) {
        read |= (uint8_t)(((port[dio_pins[n].port] >> dio_pins[n].pin) & 1U) << n);
    }
    state->pins = dio_on;
    state->outputs = dio_outputs;
    state->levels = dio_levels;
    state->read = read;
    state->groups = DIO_GROUPS_MAX;
    for (uint32_t group = 0U; group < DIO_GROUPS_MAX; group++) {
        state->group[group] = dio_group[group];
    }
}
