/** The two compiled copies of ARM's JTAG_DP entry points.
 *
 * JTAG_DP.c is built three times with its five entry points renamed each time: jtag4_* drives four
 * wires, cjtag_* drives paced OScan1 frames and cjtagf_* drives them without pacing. cjtag_dp.c
 * owns the names ARM's DAP.c calls and picks a copy per call.
 *
 * Both sets are declared here so the dispatcher and the copies cannot drift: a prototype that
 * disagreed with the definition would link and then pass arguments wrongly.
 */
#ifndef JTAG_DP_H
#define JTAG_DP_H

#include <stdint.h>

/** ARM's DAP_JTAG_TransferBlock with the transfer inlined; four-wire only. */
uint32_t JTAG_TransferBlock(const uint8_t* request, uint8_t* response);

void jtag4_Sequence(uint32_t info, const uint8_t* tdi, uint8_t* tdo);
void jtag4_IR(uint32_t ir);
uint32_t jtag4_ReadIDCode(void);
void jtag4_WriteAbort(uint32_t data);
uint8_t jtag4_Transfer(uint32_t request, uint32_t* data);

void cjtag_Sequence(uint32_t info, const uint8_t* tdi, uint8_t* tdo);
void cjtag_IR(uint32_t ir);
uint32_t cjtag_ReadIDCode(void);
void cjtag_WriteAbort(uint32_t data);
uint8_t cjtag_Transfer(uint32_t request, uint32_t* data);

// The same copy again without the pacing, for requests above the switch point.
void cjtagf_Sequence(uint32_t info, const uint8_t* tdi, uint8_t* tdo);
void cjtagf_IR(uint32_t ir);
uint32_t cjtagf_ReadIDCode(void);
void cjtagf_WriteAbort(uint32_t data);
uint8_t cjtagf_Transfer(uint32_t request, uint32_t* data);

#endif
