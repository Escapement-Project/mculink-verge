#ifndef USB_DESCRIPTORS_H
#define USB_DESCRIPTORS_H

/** Register the endpoints the descriptors declare but the controller lacks, the CDC notifications,
 *  as TinyUSB placeholders. Call before tusb_init. */
void usb_descriptors_init(void);

#endif
