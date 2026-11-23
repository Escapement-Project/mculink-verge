/** CPU 0's half of energy capture: the integration state, each segment's integration for DMA0's
 *  interrupt, and the loop's two tasks (src/main.c calls all three). src/energy_integration.h has
 *  the code both cores run on the state, and who may change what. All of it runs from SRAM, as its
 *  callers do. */
#include "energy_integration.h"

#include "dio_edges.h"

/** In SRAM0, CPU 0's own bank: DMA0's ring writes and the interrupt's reads stay off CPU 1's. */
struct energy_integration energy_integration;

/** Out of line, so tools/energy_checks.py can call it. */
__attribute__((section(".ramfunc"), noinline)) void energy_integrate_segment(bool took_a, bool took_b) {
    energy_integrate(&energy_integration, took_a, took_b);
}

/** Whether the converter has finished a pair and the next trigger is far enough off for the switch
 *  to settle (energy_monitor.c, switch_timing), by the conversion timer alone: CTIMER2 is on port
 *  7, which no timed window uses, and ADC0 shares port 10 with CPU 1's wire (BUSES.md). CTIMER2
 *  toggles its trigger output every `trigger_ticks`, and the converter starts on its rise. */
__attribute__((section(".ramfunc"))) static bool energy_between_pairs(struct energy_integration const* energy) {
    uint32_t const emr = CTIMER2->EMR;
    uint32_t const count = CTIMER2->TC;
    if (CTIMER2->EMR != emr) {
        return false;
    }
    uint32_t const since = ((emr & CTIMER_EMR_EM3_MASK) != 0U) ? (count + 1U) : (energy->trigger_ticks + count + 1U);
    return (since >= energy->rearm_from_ticks) && (since <= energy->rearm_until_ticks);
}

/** Re-arm the Pro's low range outside the DMA interrupt, once the integration has seen the high
 *  range's current stay under the low range's limit for a whole segment. Where the rate leaves
 *  room, it waits until the converter is between pairs, so the pair after the switch is the low
 *  range's alone (src/energy_pairs.h); the loop keeps coming back while it waits. */
__attribute__((section(".ramfunc"), noinline)) void energy_range_retry_task(void) {
    if (!energy_rearm_wanted()) {
        return;
    }
    bool const between = energy_integration.rearm_between_pairs;
    if (between && !energy_between_pairs(&energy_integration)) {
        return;
    }
    // Nothing may come between the check and the edge, the pin interrupts included.
    uint32_t const primask = __get_PRIMASK();
    __disable_irq();
    if (!between || energy_between_pairs(&energy_integration)) {
        dap_shared.energy_range_retry_pending = 0U;
        uint32_t const range_pins = GPIO->PIN[0];
        // The comparator keeps the reset, so a current back over the threshold keeps the high range.
        if (((range_pins & (1UL << 29U)) != 0U) && ((range_pins & (1UL << 30U)) == 0U)) {
            energy_latch_clock();
        }
    }
    __set_PRIMASK(primask);
}

/** Main-loop acknowledgement follows the previous handler's segment-count publication. */
__attribute__((section(".ramfunc"), noinline)) void energy_control_task(void) {
    uint32_t const requested = dap_shared.energy_control_request;
    if (requested != dap_shared.energy_control_ack) {
        uint32_t const basepri = cpu0_mask_all_but_edges();
        __DMB();
        dap_shared.energy_control_ack = requested;
        __DMB();
        cpu0_unmask(basepri);
    }
}
