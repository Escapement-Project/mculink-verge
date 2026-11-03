#include "usb_speed.h"

#include "tusb.h"

uint16_t usb_bulk_packet_size(void) {
    return (tud_speed_get() == TUSB_SPEED_HIGH) ? USB_BULK_HIGH_SPEED : USB_BULK_FULL_SPEED;
}
