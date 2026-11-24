/** DMA0's setup at boot and its interrupt, both CPU 0's. */
#include "dma0.h"

#include "fsl_clock.h"
#include "fsl_reset.h"

#include "dap_shared.h"
#include "energy_integration.h"
#include "swo_ring.h"
#include "vcom.h"

/** The startup file's vector table names it, weakly; this is the strong definition. */
void DMA0_IRQHandler(void);

/** DMA0 has finished a VCOM transfer or one segment of SWO or energy capture or a console's receive
 *  ring. CPU 0 counts the rings, and each reader turns a segment count into a position.
 *  Completions are cleared before the counts move, so a reader can find a ring one segment short
 *  but never long. A ring's flag not due, or both at once, means its count is lost (dma_ring_take). */
__attribute__((section(".ramfunc"))) void DMA0_IRQHandler(void) {
    // An error raises this interrupt too, and until it is cleared the interrupt comes straight
    // back, ahead of everything else on this core. SWO's is CPU 1's to report, so hand it over.
    uint32_t const energy_mask = (dap_shared.energy_control_ack & DAP_ENERGY_QUIET_FLAG) != 0U ? 0U : ENERGY_DMA_MASK;
    uint32_t const failed = DMA0->COMMON[0].ERRINT & (SWO_DMA_MASK | VCOM_DMA_MASK | energy_mask);
    if (failed != 0U) {
        DMA0->COMMON[0].ERRINT = failed;
        if ((failed & SWO_DMA_MASK) != 0U) {
            dap_shared.swo_dma_error = 1U;
        }
        if ((failed & ENERGY_DMA_MASK) != 0U) {
            dap_shared.energy_dma_error = 1U;
        }
        vcom_dma_error(failed);
    }
    // Nothing may come between reading a completion and counting it: src/swo.c's ring_start
    // waits that out, SWO_COUNT_SETTLE_US. Only the pin interrupts are above this one.
    uint32_t const primask = __get_PRIMASK();
    __disable_irq();
    uint32_t const done = DMA0->COMMON[0].INTA & (SWO_DMA_MASK | VCOM_DMA_MASK | energy_mask);
    // The rings' odd segments raise INTB (src/dma_ring.h, src/energy_monitor.c).
    uint32_t const done_b = DMA0->COMMON[0].INTB & (SWO_DMA_MASK | VCOM_DMA_RX_MASK | energy_mask);
    if ((done | done_b) == 0U) {
        __set_PRIMASK(primask);
        return;
    }
    bool const energy = ((done | done_b) & ENERGY_DMA_MASK) != 0U;
    if (energy) {
        dap_shared.energy_dma_sequence++;
        __DMB();
    }
    dma_ring_take_t const swo =
        dma_ring_take(dap_shared.swo_segments, (done & SWO_DMA_MASK) != 0U, (done_b & SWO_DMA_MASK) != 0U);
    if (swo == DMA_RING_LOST) {
        // Before the flags clear: a CPU 1 read that finds them clear then finds this.
        dap_shared.swo_lost = 1U;
        __DMB();
    }
    DMA0->COMMON[0].INTA = done;
    if (done_b != 0U) {
        DMA0->COMMON[0].INTB = done_b;
    }
    __DMB();
    if (swo == DMA_RING_ONE) {
        dap_shared.swo_segments++;
    }
    __set_PRIMASK(primask);
    if (energy) {
        energy_integrate_segment((done & ENERGY_DMA_MASK) != 0U, (done_b & ENERGY_DMA_MASK) != 0U);
        dap_shared.energy_segments++;
        __DMB();
        dap_shared.energy_dma_sequence++;
    }
    vcom_dma_done(done, done_b);
}

__attribute__((noinline)) void dma0_init(void) {
    CLOCK_EnableClock(kCLOCK_Dma0);
    RESET_PeripheralReset(kDMA0_RST_SHIFT_RSTn);
    DMA0->SRAMBASE = (uint32_t)dap_shared.dma_table;
    DMA0->CTRL = DMA_CTRL_ENABLE_MASK;
}
