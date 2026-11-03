#include "usb_speed.h"

#include <stdbool.h>

#include "fsl_device_registers.h"
#include "device/dcd.h"
#include "tusb.h"

extern volatile uint32_t system_ticks;

/** DEVCMDSTAT bit 21, "Forced FS" in UM11126's USB1 chapter: no K chirp during a reset, so the
 *  device stays at full speed. NXP's header names no mask for it. */
#define DEVCMDSTAT_FORCED_FS (1UL << 21)
/** Write-one-to-clear flags, written back as zero by a read-modify-write, as TinyUSB's driver does. */
#define DEVCMDSTAT_W1C                                                                                       \
    (USBHSD_DEVCMDSTAT_SETUP_MASK | USBHSD_DEVCMDSTAT_DCON_C_MASK | USBHSD_DEVCMDSTAT_DSUS_C_MASK |       \
     USBHSD_DEVCMDSTAT_DRES_C_MASK)
/** The controller's Speed field. */
#define SPEED_FULL 1U
#define SPEED_HIGH 2U

/** Tries at high speed after the first attach. On a full-speed-only port each one costs about
 *  RECHIRP_DOWN_MS and an enumeration. */
#define RECHIRP_TRIES 3U
/** Off the bus this long, so a host debouncing the disconnect sees it. Counted on system_ticks,
 *  which advances only while CPU 0's loop runs: usb_speed_waiting keeps the loop from waiting meanwhile,
 *  or SysTick, masked in the wait, would count only at each LED tick and the time would double. */
#define RECHIRP_DOWN_MS 300U

/** The Speed field as the last bus reset ended, read from the controller in the event hook:
 *  TinyUSB's own copy changes only once tud_task has handled the reset. 0 when there is none to
 *  look at. */
static volatile uint8_t reset_speed;
static uint8_t tries;
static bool down;
static uint32_t down_since;

uint16_t usb_bulk_packet_size(void) {
    return (tud_speed_get() == TUSB_SPEED_HIGH) ? USB_BULK_HIGH_SPEED : USB_BULK_FULL_SPEED;
}

#if defined(USB_FULL_SPEED_TEST_ALWAYS) || defined(USB_FULL_SPEED_TEST_FIRST)
static void set_forced_fs(bool forced) {
    uint32_t const status = USBHSD->DEVCMDSTAT & ~(DEVCMDSTAT_W1C | DEVCMDSTAT_FORCED_FS);
    USBHSD->DEVCMDSTAT = status | (forced ? DEVCMDSTAT_FORCED_FS : 0U);
}
#endif

void usb_speed_init(void) {
#if defined(USB_FULL_SPEED_TEST_ALWAYS) || defined(USB_FULL_SPEED_TEST_FIRST)
    set_forced_fs(true);
#endif
}

/** TinyUSB calls this for every event it queues, from the USB interrupt. */
__attribute__((section(".ramfunc"))) void tud_event_hook_cb(uint8_t rhport, uint32_t eventid, bool in_isr) {
    (void)rhport;
    (void)in_isr;
    if (eventid == DCD_EVENT_BUS_RESET) {
        reset_speed = (uint8_t)((USBHSD->DEVCMDSTAT & USBHSD_DEVCMDSTAT_Speed_MASK) >> USBHSD_DEVCMDSTAT_Speed_SHIFT);
    }
}

__attribute__((section(".ramfunc"))) bool usb_speed_waiting(void) {
    return down;
}

/** In SRAM, as the rest of CPU 0's loop is; out of line, so tools/usb_checks.py can call it. */
__attribute__((section(".ramfunc"), noinline)) void usb_speed_task(void) {
    if (down) {
        if ((uint32_t)(system_ticks - down_since) >= RECHIRP_DOWN_MS) {
            down = false;
#ifdef USB_FULL_SPEED_TEST_FIRST
            set_forced_fs(false);
#endif
            // A reset the controller saw on the idle bus while the probe was off it is not the
            // host's: only one after the reconnect may count.
            __atomic_store_n(&reset_speed, 0U, __ATOMIC_RELAXED);
            tud_connect();
        }
        return;
    }
    uint8_t const speed = __atomic_exchange_n(&reset_speed, 0U, __ATOMIC_RELAXED);
    if ((speed != SPEED_FULL) || (tries >= RECHIRP_TRIES)) {
        return;
    }
    tries++;
    down = true;
    down_since = system_ticks;
    tud_disconnect();
}
