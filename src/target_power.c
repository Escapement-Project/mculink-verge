#include "target_power.h"

#include "fsl_clock.h"
#include "fsl_device_registers.h"
#include "fsl_iocon.h"

#include "DAP_config.h"

#include "DAP.h"
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

/** Vendor 0x8F takes one mode byte: 0 off, 1 on, 2 read. It answers `status, enabled`.
 *  Status is DAP_ERROR unless target supply control is available and the mode is known. The state
 *  is the output latch's last value since startup; it does not confirm voltage at the target.
 */
uint32_t target_power_command(const uint8_t* request, uint8_t* response) {
    const uint8_t mode = request[1];
    bool ok = target_power_available();
    if (ok && mode == TARGET_POWER_MODE_OFF) {
        ok = target_power_set(false);
    } else if (ok && mode == TARGET_POWER_MODE_ON) {
        ok = target_power_set(true);
    } else if (mode != TARGET_POWER_MODE_READ) {
        ok = false;
    }
    response[1] = ok ? DAP_OK : DAP_ERROR;
    response[2] = target_power_enabled() ? 1U : 0U;
    return 3U;
}
