#ifndef USB_SPEED_H
#define USB_SPEED_H

#include <stdbool.h>
#include <stdint.h>

#define USB_BULK_HIGH_SPEED 512U
#define USB_BULK_FULL_SPEED 64U

/** The bulk packet size at the speed the host settled on. */
uint16_t usb_bulk_packet_size(void);

/** Before tusb_init. Only a full-speed test build (USB_FULL_SPEED_TEST) does anything here. */
void usb_speed_init(void);

/** From CPU 0's main loop. When a bus reset leaves the probe at full speed, it leaves the bus and
 *  attaches again to try high speed, three times after power-up. It works at full speed after that. */
void usb_speed_task(void);

/** Whether the probe is off the bus for a retry, timing it: CPU 0's loop must not wait meanwhile. */
bool usb_speed_waiting(void);

#endif
