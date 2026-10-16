/** The SWD transfer block that bypasses the per-word call into SWD_Transfer.
 *
 * Declared apart from DAP.h because it is ours rather than ARM's. src/dap_dispatch.c routes a
 * block command here; everything else goes to ARM's command processing.
 */
#ifndef SWD_DP_H
#define SWD_DP_H

#include <stdint.h>

/** Returns what ARM's DAP_TransferBlock returns: response bytes low, request bytes high. */
uint32_t SWD_TransferBlock(const uint8_t* request, uint8_t* response);

/** The same for DAP_Transfer, expanded once per clock path so the transfer inlines into it. */
uint32_t SWD_TransferCommand(const uint8_t* request, uint8_t* response);

#endif
