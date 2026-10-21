#ifndef VCOM_H
#define VCOM_H

#include <stdbool.h>
#include <stdint.h>

/** DMA0's channels for Flexcomm 0 and 1, receive then transmit, fixed by UM11126 table 413. */
#define VCOM_DMA_RX_CHANNEL(flexcomm) (4U + (2U * (flexcomm)))
#define VCOM_DMA_TX_CHANNEL(flexcomm) (5U + (2U * (flexcomm)))
#define VCOM_DMA_MASK (0xFUL << VCOM_DMA_RX_CHANNEL(0U))

/** Bring up the target consoles that exist on this board (board_straps.h). Call after board_init, which
 *  also claims Flexcomm 0, after board_read_straps, and after DMA0 is enabled with its table. */
void vcom_init(void);

/** Move bytes between each console's USB CDC interface and its UART. */
void vcom_task(void);

/** Whether vcom_task has bytes it could move now, the only time CPU 0's loop runs it. Runs from
 *  SRAM, for CPU 0's wait in main.c. */
bool vcom_ready(void);

/** CPU 0's DMA interrupt: the completions of `done` (DMA0 INTA bits), already cleared. */
void vcom_dma_done(uint32_t done);

#endif
