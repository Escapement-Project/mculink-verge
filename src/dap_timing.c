#include "dap_timing.h"

#ifdef DAP_TIMING_DIAGNOSTICS

#include "fsl_clock.h"
#include "fsl_reset.h"

#include "dap_shared.h"

void dap_timing_init(void) {
    CLOCK_AttachClk(kFRO_HF_to_CTIMER0);
    CLOCK_EnableClock(kCLOCK_Timer0);
    RESET_PeripheralReset(kCTIMER0_RST_SHIFT_RSTn);
    CTIMER0->TCR = CTIMER_TCR_CRST_MASK;
    CTIMER0->PR = 0U;
    CTIMER0->MCR = 0U;
    CTIMER0->IR = 0x0FU;
    dap_stage.timing.rx_count = 0U;
    dap_stage.timing.rx_callback_cycles = 0U;
    dap_stage.timing.rx_stage_copy_cycles = 0U;
    dap_stage.timing.tx_stage_copy_cycles = 0U;
    dap_stage.timing.rx_publish_cycles = 0U;
    dap_stage.timing.mailbox1_signal_cycles = 0U;
    dap_stage.timing.wake_to_dispatch_cycles = 0U;
    dap_stage.timing.dispatch_cycles = 0U;
    dap_stage.timing.request_copy_cycles = 0U;
    dap_stage.timing.dap_execute_cycles = 0U;
    dap_stage.timing.response_copy_cycles = 0U;
    dap_stage.timing.reply_publish_cycles = 0U;
    dap_stage.timing.tx_count = 0U;
    dap_stage.timing.tx_start_cycles = 0U;
    dap_stage.timing.tx_in_flight_cycles = 0U;
    for (uint32_t slot = 0U; slot < REQUEST_SLOTS; slot++) {
        dap_stage.request_tick[slot] = 0U;
        dap_stage.response_tx_tick[slot] = 0U;
        dap_stage.response_timing_excluded[slot] = 0U;
    }
    dap_stage.timing.clock_hz = CLOCK_GetFroHfFreq();
    CTIMER0->TCR = CTIMER_TCR_CEN_MASK;
}

#endif
