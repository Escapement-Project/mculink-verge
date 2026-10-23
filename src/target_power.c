#include "target_power.h"

#include "fsl_clock.h"
#include "fsl_device_registers.h"
#include "fsl_iocon.h"

#include "board_pins.h"

#define TARGET_POWER_PORT 0U
#define TARGET_POWER_PIN 8U
#define TARGET_POWER_MASK (1UL << TARGET_POWER_PIN)
#define TARGET_POWER_PIN_MODE (IOCON_FUNC0 | IOCON_MODE_INACT | IOCON_DIGITAL_EN)

static bool target_power_ready;
static bool target_power_on;

__attribute__((noinline)) void target_power_init(void) {
    target_power_ready = false;
    target_power_on = false;
    if (board_kind != BOARD_KIND_PRO) {
        return;
    }

    CLOCK_EnableClock(kCLOCK_Gpio0);
    // The TPS736 enable is active high; preload its disabled level before driving the pin.
    GPIO->CLR[TARGET_POWER_PORT] = TARGET_POWER_MASK;
    GPIO->DIRSET[TARGET_POWER_PORT] = TARGET_POWER_MASK;
    IOCON_PinMuxSet(IOCON, TARGET_POWER_PORT, TARGET_POWER_PIN, TARGET_POWER_PIN_MODE);
    target_power_ready = true;
}

bool target_power_set(bool enabled) {
    if (!target_power_ready) {
        return false;
    }

    if (enabled) {
        GPIO->SET[TARGET_POWER_PORT] = TARGET_POWER_MASK;
    } else {
        GPIO->CLR[TARGET_POWER_PORT] = TARGET_POWER_MASK;
    }
    target_power_on = enabled;
    return true;
}

bool target_power_enabled(void) {
    return target_power_on;
}

bool target_power_available(void) {
    return target_power_ready;
}
