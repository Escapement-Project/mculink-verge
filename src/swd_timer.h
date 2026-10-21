/** SWCLK from CTimer 0, for SWD reads at 50 MHz.
 *
 * SWCLK's pin is also CTimer 0's match 0 output, which makes the clock in a run while the core only
 * stores and samples SWDIO between its edges. A run ends without the core: CTimer 2 or CTimer 4,
 * started beside CTimer 0, matches after a fixed count of cycles, and DMA1 writes CTimer 0's TCR
 * to hold its count at zero. SWCLK falls there and stays low, and every run starts from the same
 * count however late in a period the stop landed. src/swd_gen.c generates the code, and
 * TIMERGEN.md has the measurements behind it.
 */
#ifndef SWD_TIMER_H
#define SWD_TIMER_H

#include <stddef.h>
#include <stdint.h>

#include "fsl_device_registers.h"

#define SWD_TIMER_CLOCK ((uint32_t)CTIMER0)
/** The stop for the run that ends on the acknowledge, and for the one that ends on the data. */
#define SWD_TIMER_STOP_ACK ((uint32_t)CTIMER2)
#define SWD_TIMER_STOP_DATA ((uint32_t)CTIMER4)
#define SWD_TIMER_TCR ((uint32_t)offsetof(CTIMER_Type, TCR))
#define SWD_TIMER_PIN ((uint32_t)&IOCON->PIO[0][0])
/** SWCLK's pin function that is CTimer 0's match 0. */
#define SWD_TIMER_PIN_FUNC 3U

/** Configures the clock, both stops and their DMA, the stops matching `ack` and `data` cycles
 * after they start, and leaves CTimer 0 stopped as a run leaves it. */
void swd_timer_setup(uint32_t ack, uint32_t data);

/** SWCLK's pin configuration outside a run. */
static inline uint32_t swd_timer_pin_gpio(void) { return IOCON->PIO[0][0]; }

#endif
