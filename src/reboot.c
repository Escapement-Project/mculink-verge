#include "reboot.h"

#include "bsp/board_api.h"
#include "fsl_device_registers.h"
#include "dap_shared.h"
#include "reboot_usb.h"
#include "usb_transport.h"

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

extern volatile uint32_t system_ticks;

static volatile uint8_t usb_pending = REBOOT_NONE;
static uint8_t active_mode = REBOOT_NONE;
static bool active_from_usb;
static uint32_t disconnect_started;
static bool disconnect_requested;

void reboot_clear_boot_mode(void) {
    PMC->AOREG1 &= ~AOREG1_BOOT_MODE_MASK;
}

void reboot_request_from_usb(uint8_t mode) {
    usb_pending = mode;
}

void reboot_poll(void) {
    if (usb_pending != REBOOT_NONE) {
        active_mode = usb_pending;
        usb_pending = REBOOT_NONE;
        active_from_usb = true;
    } else if (active_mode == REBOOT_NONE) {
        // CPU 1 publishes the mode after its reply is READY, so the wait below sees that reply.
        // A faulting CPU 1 can leave anything here, and only the two modes may reset the probe.
        uint8_t const mode = dap_shared.reboot_mode;
        if ((mode == REBOOT_FIRMWARE) || (mode == REBOOT_BOOTLOADER)) {
            active_mode = mode;
            active_from_usb = false;
        }
        if (mode != REBOOT_NONE) {
            dap_shared.reboot_mode = REBOOT_NONE;
        }
    }

    if (active_mode == REBOOT_NONE ||
        reboot_wait_for_response(active_from_usb, usb_transport_response_idle())) {
        return;
    }

    if (!usb_transport_disconnect_complete()) {
        if (!disconnect_requested) {
            usb_transport_request_disconnect();
            disconnect_started = system_ticks;
            disconnect_requested = true;
        }
        return;
    }

    // Keep the USB disconnect visible to the host before resetting.
    if ((uint32_t)(system_ticks - disconnect_started) < 20U) {
        return;
    }

    if (active_mode == REBOOT_BOOTLOADER) {
        PMC->AOREG1 |= AOREG1_BOOT_MODE_ISP;
    }
    NVIC_SystemReset();
}
