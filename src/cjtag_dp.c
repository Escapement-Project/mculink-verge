/** The JTAG entry points, routed to four-wire or OScan1.
 *
 * ARM's JTAG_DP.c is compiled twice, with its five entry points renamed each time: once normally
 * as `jtag4_*`, and once through src/cjtag_jtag_dp.c as `cjtag_*`, where the wire layer is
 * replaced by OScan1 frames. This file owns the real names and picks a path once per call, so
 * neither path carries a per-bit branch and neither duplicates the other's TAP logic.
 */
// DAP_config.h first: DAP.h reads the DAP_SWD and DAP_JTAG settings it defines, so a sorted
// include block that puts DAP.h ahead of it will not compile.
#include "DAP_config.h"

#include "DAP.h"
#include "cjtag.h"

extern void jtag4_Sequence(uint32_t info, const uint8_t* tdi, uint8_t* tdo);
extern void jtag4_IR(uint32_t ir);
extern uint32_t jtag4_ReadIDCode(void);
extern void jtag4_WriteAbort(uint32_t data);
extern uint8_t jtag4_Transfer(uint32_t request, uint32_t* data);

extern void cjtag_Sequence(uint32_t info, const uint8_t* tdi, uint8_t* tdo);
extern void cjtag_IR(uint32_t ir);
extern uint32_t cjtag_ReadIDCode(void);
extern void cjtag_WriteAbort(uint32_t data);
extern uint8_t cjtag_Transfer(uint32_t request, uint32_t* data);

void JTAG_Sequence(uint32_t info, const uint8_t* tdi, uint8_t* tdo) {
    if (cJTAG_Active()) {
        cjtag_Sequence(info, tdi, tdo);
    } else {
        jtag4_Sequence(info, tdi, tdo);
    }
}

void JTAG_IR(uint32_t ir) {
    if (cJTAG_Active()) {
        cjtag_IR(ir);
    } else {
        jtag4_IR(ir);
    }
}

uint32_t JTAG_ReadIDCode(void) { return cJTAG_Active() ? cjtag_ReadIDCode() : jtag4_ReadIDCode(); }

void JTAG_WriteAbort(uint32_t data) {
    if (cJTAG_Active()) {
        cjtag_WriteAbort(data);
    } else {
        jtag4_WriteAbort(data);
    }
}

uint8_t JTAG_Transfer(uint32_t request, uint32_t* data) {
    return cJTAG_Active() ? cjtag_Transfer(request, data) : jtag4_Transfer(request, data);
}
