#ifndef USB_SPEED_H
#define USB_SPEED_H

#include <stdint.h>

#define USB_BULK_HIGH_SPEED 512U
#define USB_BULK_FULL_SPEED 64U

/** The bulk packet size at the speed the host settled on. */
uint16_t usb_bulk_packet_size(void);

#endif
