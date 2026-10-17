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
#include "jtag_dp.h"
#ifdef JTAG_CODEGEN
#include "jtag_gen.h"
#endif

/** Which of the three copies a call goes to: four-wire, or two-pin paced or not.
 *
 * The two-pin choice is made here rather than inside the frame because making it there costs a
 * cycle in the paced path, which is enough to take a 10 MHz request past the clock the host asked
 * for. Once per call it costs nothing measurable.
 */
#define CJTAG_PICK(name) (DAP_Data.fast_clock ? cjtagf_##name : cjtag_##name)

/** The generated clocks when they could be built for this clock and wire, ARM's copies when not. */
#ifdef JTAG_CODEGEN
#define CJTAG_ROUTE(name) (jtag_gen_ready(JTAG_GEN_TWO_PIN) ? jgen_##name : CJTAG_PICK(name))
#define JTAG4_ROUTE(name) (jtag_gen_ready(JTAG_GEN_FOUR_WIRE) ? jgen_##name : jtag4_##name)
#else
#define CJTAG_ROUTE(name) CJTAG_PICK(name)
#define JTAG4_ROUTE(name) jtag4_##name
#endif

void JTAG_Sequence(uint32_t info, const uint8_t* tdi, uint8_t* tdo) {
    if (cJTAG_Active()) {
        CJTAG_ROUTE(Sequence)(info, tdi, tdo);
    } else {
        JTAG4_ROUTE(Sequence)(info, tdi, tdo);
    }
}

void JTAG_IR(uint32_t ir) {
    if (cJTAG_Active()) {
        CJTAG_ROUTE(IR)(ir);
    } else {
        JTAG4_ROUTE(IR)(ir);
    }
}

uint32_t JTAG_ReadIDCode(void) { return cJTAG_Active() ? CJTAG_ROUTE(ReadIDCode)() : JTAG4_ROUTE(ReadIDCode)(); }

void JTAG_WriteAbort(uint32_t data) {
    if (cJTAG_Active()) {
        CJTAG_ROUTE(WriteAbort)(data);
    } else {
        JTAG4_ROUTE(WriteAbort)(data);
    }
}

uint8_t JTAG_Transfer(uint32_t request, uint32_t* data) {
    return cJTAG_Active() ? CJTAG_ROUTE(Transfer)(request, data) : JTAG4_ROUTE(Transfer)(request, data);
}
