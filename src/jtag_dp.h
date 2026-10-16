/** The two compiled copies of ARM's JTAG_DP entry points.
 *
 * JTAG_DP.c is built twice with its five entry points renamed each time: jtag4_* drives four
 * wires, cjtag_* drives OScan1 frames. cjtag_dp.c owns the names ARM's DAP.c calls and picks a
 * copy per call.
 *
 * Both sets are declared here so the dispatcher and the copies cannot drift: a prototype that
 * disagreed with the definition would link and then pass arguments wrongly.
 */
#ifndef JTAG_DP_H
#define JTAG_DP_H

#include <stdint.h>

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

#endif
