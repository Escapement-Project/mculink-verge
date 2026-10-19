#ifndef USB_TRANSPORT_H
#define USB_TRANSPORT_H

#include <stdbool.h>

bool usb_transport_response_idle(void);
bool usb_transport_reply_in_flight(void);
void usb_transport_request_disconnect(void);
bool usb_transport_disconnect_complete(void);

#endif
