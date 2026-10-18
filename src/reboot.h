/** Reset the probe on the host's asking, optionally into the ROM bootloader.
 *
 * Every reflash so far has cost a pair of hands: fit the ISP jumper, plug in, flash, unplug,
 * remove the jumper, plug in. That is the one step in a measure-change-measure loop that cannot
 * be scripted, and it is the reason this exists. The jumper stays the recovery path for an image
 * that will not enumerate, which is exactly the case this command cannot help with.
 */
#ifndef REBOOT_H
#define REBOOT_H

#include <stdint.h>

#define REBOOT_FIRMWARE 0U
#define REBOOT_BOOTLOADER 1U

/** Clear a boot mode left in the always-on register. Call once, early, before anything resets.
 *
 * Without this a unit that was sent to the bootloader would go back there on every reset until it
 * was unplugged, because the register survives a system reset and only a power-on clears it.
 */
void reboot_clear_boot_mode(void);

/** Ask for a reset. It happens in reboot_poll, after the current reply has reached the host. */
void reboot_request(uint8_t mode);

/** Request a reset from CPU 0's USB control path, independent of DAP execution on CPU 1. */
void reboot_request_from_usb(uint8_t mode);

/** Carry out a pending request. Call from the main loop, below the command and USB tasks. */
void reboot_poll(void);

#endif
