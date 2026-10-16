#include "reboot.h"

#include "bsp/board_api.h"
#include "fsl_device_registers.h"
#include "tusb.h"

/** The ROM reads AOREG1's boot mode field out of reset, and 0x0A there means auto ISP.
 *
 * `runBootloader` in the ROM API tree is the documented route and does not work on this part:
 * erratum ISP.1 in the LPC55S6x errata sheet says so for every device from date code 2101, and
 * names this register as the workaround. AOREG1 is in the always-on power domain, so what is
 * written here survives the system reset that follows it.
 */
#define AOREG1_BOOT_MODE_ISP (0x0AUL << 16)

/** The boot mode field. The bits below it hold reset causes that POWER_GetResetCause reads, so
 *  clearing the mode has to leave them alone. */
#define AOREG1_BOOT_MODE_MASK (0xFFUL << 16)

/** No request pending. Any value outside the two modes would do; this one cannot be a mode. */
#define REBOOT_NONE 0xFFU

static volatile uint8_t pending = REBOOT_NONE;

void reboot_clear_boot_mode(void) {
    PMC->AOREG1 &= ~AOREG1_BOOT_MODE_MASK;
}

void reboot_request(uint8_t mode) {
    pending = mode;
}

void reboot_poll(void) {
    if (pending == REBOOT_NONE) {
        return;
    }
    const uint8_t mode = pending;
    pending = REBOOT_NONE;

    // board_delay runs tud_task while it waits, so the reply to the command that asked for this
    // actually leaves the endpoint. Then drop off the bus deliberately: a host that sees a
    // disconnect re-enumerates cleanly, where one whose device simply stops answering reports a
    // transfer error first.
    board_delay(20);
    tud_disconnect();
    board_delay(20);

    if (mode == REBOOT_BOOTLOADER) {
        PMC->AOREG1 |= AOREG1_BOOT_MODE_ISP;
    }
    NVIC_SystemReset();
}
