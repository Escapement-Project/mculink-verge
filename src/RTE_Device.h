/** Configuration for NXP's CMSIS-Driver USART.
 *
 * Only Flexcomm 3 is enabled, because that is where SWO arrives: TDO/SWO is P0_3, whose only
 * serial function is FC3_RXD.
 *
 * No DMA. The SWO stream is byte-at-a-time into a ring buffer and the transfers are short, so a
 * DMA channel would buy nothing and would be one more thing to arbitrate with the USB stack.
 */
#ifndef RTE_DEVICE_H
#define RTE_DEVICE_H

#include "swo_board.h"

#define RTE_USART3 1
#define RTE_USART3_DMA_EN 0

/* P0_3 is shared with JTAG TDO, so the mux follows the driver's own lifecycle. */
#define RTE_USART3_PIN_INIT board_swo_pin_init
#define RTE_USART3_PIN_DEINIT board_swo_pin_deinit

#endif
