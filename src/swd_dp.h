/** The SWD transfer commands over the generated transfers, in product images (src/swd_dp.c).
 *
 * Declared apart from DAP.h because they are ours rather than ARM's. src/dap_dispatch.c routes
 * DAP_Transfer and DAP_TransferBlock here; everything else goes to ARM's command processing.
 */
#ifndef SWD_DP_H
#define SWD_DP_H

#include <stdint.h>

/** Returns what ARM's DAP_TransferBlock returns: response bytes low, request bytes high. */
uint32_t SWD_TransferBlock(const uint8_t* request, uint8_t* response);

/** The same for DAP_Transfer. */
uint32_t SWD_TransferCommand(const uint8_t* request, uint8_t* response);

#endif
