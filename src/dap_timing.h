#ifndef DAP_TIMING_H
#define DAP_TIMING_H

#ifdef DAP_TIMING_DIAGNOSTICS

#include <stdint.h>

#include "fsl_device_registers.h"

#define DAP_TIMING_COMMAND 0x93U

void dap_timing_init(void);

static inline uint32_t dap_timing_now(void) {
    return CTIMER0->TC;
}

#endif

#endif
