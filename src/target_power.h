/** MCU-Link Pro's switched target supply. See target_power.c. */
#ifndef TARGET_POWER_H
#define TARGET_POWER_H

#include <stdbool.h>

#define TARGET_POWER_MODE_OFF 0U
#define TARGET_POWER_MODE_ON 1U
#define TARGET_POWER_MODE_READ 2U

/** Configure the Pro's supply control and leave the regulator disabled. */
void target_power_init(void);

/** Set the Pro's supply state. Returns false on boards without a switched supply. */
[[nodiscard]] bool target_power_set(bool enabled);

/** The last state requested since boot. */
bool target_power_enabled(void);

/** Whether this image has an MCU-Link Pro supply control. */
bool target_power_available(void);

#endif
