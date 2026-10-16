#ifndef SWO_BOARD_H
#define SWO_BOARD_H

#include <stdint.h>

/** Flexcomm 3's clock, which NXP's CMSIS-Driver needs to compute a baud divider. */
uint32_t USART3_GetFreq(void);

/** Hand P0_3 to Flexcomm 3 so trace can be captured. It is JTAG's TDO otherwise. */
void board_swo_pin_init(void);

/** Give P0_3 back to the TAP as a GPIO input. */
void board_swo_pin_deinit(void);

#endif
