/** The JTAG entry points behind the generated code, product images only (src/cjtag_dp.c). */
#ifndef JTAG_DP_H
#define JTAG_DP_H

#include <stdint.h>

uint32_t JTAG_TransferCommand(const uint8_t* request, uint8_t* response);

// The generated paths for the wire in use, src/jtag_gen_dp.c.
void jgen_Sequence(uint32_t info, const uint8_t* tdi, uint8_t* tdo);
void jgen_IR(uint32_t ir);
uint32_t jgen_ReadIDCode(void);
void jgen_WriteAbort(uint32_t data);
uint8_t jgen_Transfer(uint32_t request, uint32_t* data);
uint32_t jgen_TransferBlock(const uint8_t* request, uint8_t* response);
/** Entering a format starts the TDI that frames carry over at 1, as ARM's cJTAG restates it and as
 * PORT_JTAG_SETUP leaves the conventional pin. */
void jgen_RestateTdi(void);

#endif
