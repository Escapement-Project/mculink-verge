#ifndef VCOM_H
#define VCOM_H

#include <stdbool.h>

/** Bring up the target console UART. Call after board_init, which also claims Flexcomm 0. */
void vcom_init(void);

/** Move bytes between the USB CDC interface and the target's UART. */
void vcom_task(void);

/** Whether vcom_task has bytes it could move now, the only time CPU 0's loop runs it. Runs from
 *  SRAM, for CPU 0's wait in main.c. */
bool vcom_ready(void);

#endif
