/** The JTAG entry points, routed to four-wire or two-pin.
 *
 * ARM's JTAG_DP.c is compiled several times, with its five entry points renamed each time: once
 * normally as `jtag4_*`, and through src/cjtag_jtag_dp.c as `cjtag_*`, `cjtagf_*` and `cjtagt_*`,
 * where the wire layer is replaced by OScan1, OScan2, OScan5 or OScan6 frames (src/jtag_dp.h). This file owns the
 * real names and picks a path once per call, so no path carries a per-bit branch and none
 * duplicates another's TAP logic.
 */
// DAP_config.h first: DAP.h reads the DAP_SWD and DAP_JTAG settings it defines, so a sorted
// include block that puts DAP.h ahead of it will not compile.
#include "DAP_config.h"

#include "DAP.h"
#include "cjtag.h"
#include "hot.h"
#include "jtag_dp.h"
#ifdef JTAG_CODEGEN
#include "jtag_gen.h"
#endif

/** Which of the copies a call goes to: four-wire, OScan1 paced or not, or the TAP-tracked one.
 *
 * The OScan1 choice is made here rather than inside the frame because making it there costs a
 * cycle in the paced path, which is enough to take a 10 MHz request past the clock the host asked
 * for. Once per call it costs nothing measurable.
 */
#define CJTAG_PICK(name) \
    (cJTAG_Format() != CJTAG_OSCAN1 ? cjtagt_##name : DAP_Data.fast_clock ? cjtagf_##name : cjtag_##name)

/** The generated clocks when they could be built for this clock and wire, ARM's copies when not. */
#ifdef JTAG_CODEGEN
#define CJTAG_ROUTE(name) (jtag_gen_ready(jtag_gen_wire()) ? jgen_##name : CJTAG_PICK(name))
#define JTAG4_ROUTE(name) (jtag_gen_ready(JTAG_GEN_FOUR_WIRE) ? jgen_##name : jtag4_##name)
// A transfer builds the transfers too, and so does an IR scan, which ARM's DAP_JTAG_Transfer sends
// just before its transfers: a chain change inside the command then costs one generation, not two.
// Everything else needs the scans alone.
#define CJTAG_ROUTE_TRANSFERS(name) (jtag_gen_ready_transfers(jtag_gen_wire()) ? jgen_##name : CJTAG_PICK(name))
#define JTAG4_ROUTE_TRANSFERS(name) (jtag_gen_ready_transfers(JTAG_GEN_FOUR_WIRE) ? jgen_##name : jtag4_##name)

uint32_t jtag_gen_wire(void) {
    if (!cJTAG_Active()) {
        return JTAG_GEN_FOUR_WIRE;
    }
    switch (cJTAG_Format()) {
    case CJTAG_OSCAN2:
        return JTAG_GEN_OSCAN2;
    case CJTAG_OSCAN5:
        return JTAG_GEN_OSCAN5;
    case CJTAG_OSCAN6:
        return JTAG_GEN_OSCAN6;
    default:
        return JTAG_GEN_TWO_PIN;
    }
}
#else
#define CJTAG_ROUTE(name) CJTAG_PICK(name)
#define JTAG4_ROUTE(name) jtag4_##name
#define CJTAG_ROUTE_TRANSFERS(name) CJTAG_PICK(name)
#define JTAG4_ROUTE_TRANSFERS(name) jtag4_##name
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
        CJTAG_ROUTE_TRANSFERS(IR)(ir);
    } else {
        JTAG4_ROUTE_TRANSFERS(IR)(ir);
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
    return cJTAG_Active() ? CJTAG_ROUTE_TRANSFERS(Transfer)(request, data) : JTAG4_ROUTE_TRANSFERS(Transfer)(request, data);
}

struct jtag_transfer_fns {
    void (*ir)(uint32_t ir);
    uint8_t (*transfer)(uint32_t request, uint32_t* data);
};

static struct jtag_transfer_fns resolve_transfers(void) {
#ifdef JTAG_CODEGEN
    if (jtag_gen_ready_transfers(jtag_gen_wire())) {
        return (struct jtag_transfer_fns){jgen_IR, jgen_Transfer};
    }
#endif
    if (cJTAG_Active()) {
        return (struct jtag_transfer_fns){CJTAG_PICK(IR), CJTAG_PICK(Transfer)};
    }
    return (struct jtag_transfer_fns){jtag4_IR, jtag4_Transfer};
}

#include "jtag_transfer.h"
