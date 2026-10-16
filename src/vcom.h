#ifndef VCOM_H
#define VCOM_H

/** Bring up the target console UART. Call after board_init, which also claims Flexcomm 0. */
void vcom_init(void);

/** Move bytes between the USB CDC interface and the target's UART. */
void vcom_task(void);

#endif
