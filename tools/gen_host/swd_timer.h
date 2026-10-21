/* Stand-in for src/swd_timer.h: the generated code only needs the addresses, and the host has no
 * timers to configure. */
#ifndef GEN_HOST_SWD_TIMER_H
#define GEN_HOST_SWD_TIMER_H

#include <stdint.h>

#define SWD_TIMER_CLOCK 0x40008000U
#define SWD_TIMER_STOP_ACK 0x40028000U
#define SWD_TIMER_STOP_DATA 0x4002A000U
#define SWD_TIMER_TCR 4U
#define SWD_TIMER_PIN 0x40001000U
#define SWD_TIMER_PIN_FUNC 3U

static inline void swd_timer_setup(uint32_t ack, uint32_t data) {
    (void)ack;
    (void)data;
}

static inline uint32_t swd_timer_pin_gpio(void) { return 0x0000010AU; }

#endif
