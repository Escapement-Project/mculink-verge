#include "swd_timer.h"

#include <stddef.h>

#include "fsl_clock.h"

/* DMA1 control bits (UM11126 chapter 13). */
#define CFG_HWTRIGEN (1U << 1)
#define CFG_TRIGPOL (1U << 4)
#define XFER_CFGVALID (1U << 0)
#define XFER_RELOAD (1U << 1)
#define XFER_CLRTRIG (1U << 3)
#define XFER_WIDTH_32 (2U << 8)
/** One word, no increment either side, and the same again on the next trigger. */
#define XFER (XFER_CFGVALID | XFER_RELOAD | XFER_CLRTRIG | XFER_WIDTH_32)

/* CTimer bits. */
#define TCR_CEN (1U << 0)
#define TCR_CRST (1U << 1)
#define MCR_MR0R (1U << 1)
#define MCR_MR0S (1U << 2)
#define MCR_MR3R (1U << 10)

/** DMA1's trigger inputs for CTimer 2 and CTimer 4 match 0 (UM11126's DMA1 input mux table).
 * The SDK's fsl_inputmux_connections.h calls input 7 CTimer 4's match 1 in some versions; it is
 * match 0, the one the stop's MR0 drives. */
#define ITRIG_CTIMER2_M0 6U
#define ITRIG_CTIMER4_M0 7U

struct descriptor {
    uint32_t xfercfg;
    uint32_t src_end;
    uint32_t dst_end;
    uint32_t next;
};

/* DMA1 reads channel n's first descriptor at SRAMBASE + 16n, and the table has to be 512-byte
 * aligned. Each channel's descriptor reloads one that points at itself, so the channel is armed
 * again as soon as it has stopped the clock. */
static struct descriptor table[2] __attribute__((aligned(512)));
static struct descriptor reload[2] __attribute__((aligned(16)));
/** What DMA1 writes to CTimer 0's TCR: stopped, with its count held at zero. In SRAM, which nothing
 * else touches during a run, so that DMA1 takes the same time to read it every time. */
static uint32_t stop;

static void stop_setup(CTIMER_Type* timer, uint32_t cycles) {
    timer->MR[0] = cycles;
    // Back to zero and stopped on the match, so a start is all the next run needs.
    timer->MCR = MCR_MR0R | MCR_MR0S;
    timer->CTCR = 0U;
    timer->PR = 0U;
    timer->PWMC = 0U;
    timer->TCR = TCR_CRST;
    timer->TCR = 0U;
    timer->IR = 0xFFU;
}

__attribute__((noinline)) void swd_timer_setup(uint32_t ack, uint32_t data) {
    // Nothing else uses these peripherals, and every run leaves them as this does. The latency
    // measurement at connect builds dozens of times.
    static uint32_t done_ack;
    static uint32_t done_data;
    if ((ack == done_ack) && (data == done_data)) {
        return;
    }
    done_ack = ack;
    done_data = data;
    CLOCK_EnableClock(kCLOCK_InputMux);
    CLOCK_EnableClock(kCLOCK_Dma1);
    CLOCK_EnableClock(kCLOCK_Timer0);
    CLOCK_EnableClock(kCLOCK_Timer2);
    CLOCK_EnableClock(kCLOCK_Timer4);
    // The core's clock: the generated code counts the timers' ticks as its own cycles.
    CLOCK_AttachClk(kMAIN_CLK_to_CTIMER0);
    CLOCK_AttachClk(kMAIN_CLK_to_CTIMER2);
    CLOCK_AttachClk(kMAIN_CLK_to_CTIMER4);

    // Period 3 with match 0 at 1: low for one cycle, high for two. The other way round, the shape of
    // the GPIO path's writes, the target missed every other read.
    CTIMER0->TCR = TCR_CRST;
    CTIMER0->CTCR = 0U;
    CTIMER0->PR = 0U;
    CTIMER0->MR[3] = 2U;
    CTIMER0->MR[0] = 1U;
    CTIMER0->MCR = MCR_MR3R;
    CTIMER0->EMR = 0U;
    CTIMER0->PWMC = 1U;
    CTIMER0->TCR = 0U;
    stop_setup(CTIMER2, ack);
    stop_setup(CTIMER4, data);

    stop = TCR_CRST;
    const uint32_t tcr = (uint32_t)&CTIMER0->TCR;
    for (uint32_t ch = 0U; ch < 2U; ch++) {
        reload[ch] = (struct descriptor){XFER, (uint32_t)&stop, tcr, (uint32_t)&reload[ch]};
        table[ch] = (struct descriptor){0U, (uint32_t)&stop, tcr, (uint32_t)&reload[ch]};
    }
    INPUTMUX->DMA1_ITRIG_INMUX[0] = ITRIG_CTIMER2_M0;
    INPUTMUX->DMA1_ITRIG_INMUX[1] = ITRIG_CTIMER4_M0;
    DMA1->SRAMBASE = (uint32_t)table;
    DMA1->CTRL = 1U;
    for (uint32_t ch = 0U; ch < 2U; ch++) {
        DMA1->CHANNEL[ch].CFG = CFG_HWTRIGEN | CFG_TRIGPOL;
        DMA1->CHANNEL[ch].XFERCFG = XFER;
    }
    __DSB();
    DMA1->COMMON[0].ENABLESET = 3U;
    // A run leaves its stop at the match, and a start from there counts one cycle more than a
    // start from zero. Every run starts from the match, the first too. Whatever that sets off
    // lands on CTimer 0 while it is stopped anyway, and CTimer 0 is left as a run leaves it.
    CTIMER2->TC = ack;
    CTIMER4->TC = data;
    for (volatile uint32_t i = 0U; i < 64U; i++) {
    }
    CTIMER0->TCR = TCR_CRST;
}
