/** The JTAG entry points, routed to the generated code for the wire in use.
 *
 * Only the ARM reference build (both generators off) has ARM's copies: JTAG_DP.c compiled with its
 * five entry points renamed, once as `jtag4_*` and through src/cjtag_jtag_dp.c as `cjtag_*`,
 * `cjtagf_*` and `cjtagt_*`, where the wire layer is replaced by OScan1, OScan2, OScan5 or OScan6
 * frames (src/jtag_dp.h). This file owns the real names and picks a path once per call, so no
 * path carries a per-bit branch and none duplicates another's TAP logic.
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

#ifdef JTAG_CODEGEN
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

/* This build has no ARM copy behind the generated code. Where nothing could be generated, these
 * leave the wire alone. The commands built on the ones with nothing to report are refused before
 * they start (src/dap_dispatch.c), so these are reached only when that check was not made. */
static void refused_Sequence(uint32_t info, const uint8_t* tdi, uint8_t* tdo) {
    (void)info;
    (void)tdi;
    (void)tdo;
}
static void refused_IR(uint32_t ir) { (void)ir; }
static uint32_t refused_ReadIDCode(void) { return 0U; }
static void refused_WriteAbort(uint32_t data) { (void)data; }
static uint8_t refused_Transfer(uint32_t request, uint32_t* data) {
    (void)request;
    (void)data;
    return DAP_TRANSFER_ERROR;
}

#define ROUTE(name) (jtag_gen_ready(jtag_gen_wire()) ? jgen_##name : refused_##name)
// A transfer builds the transfers too, and so does an IR scan, which ARM's DAP_JTAG_Transfer sends
// just before its transfers: a chain change inside the command then costs one generation, not two.
// Everything else needs the scans alone.
#define ROUTE_TRANSFERS(name) (jtag_gen_ready_transfers(jtag_gen_wire()) ? jgen_##name : refused_##name)

void JTAG_Sequence(uint32_t info, const uint8_t* tdi, uint8_t* tdo) { ROUTE(Sequence)(info, tdi, tdo); }

void JTAG_IR(uint32_t ir) { ROUTE_TRANSFERS(IR)(ir); }

uint32_t JTAG_ReadIDCode(void) { return ROUTE(ReadIDCode)(); }

void JTAG_WriteAbort(uint32_t data) { ROUTE(WriteAbort)(data); }

uint8_t JTAG_Transfer(uint32_t request, uint32_t* data) { return ROUTE_TRANSFERS(Transfer)(request, data); }

struct jtag_transfer_fns {
    void (*ir)(uint32_t ir);
    uint8_t (*transfer)(uint32_t request, uint32_t* data);
};

static struct jtag_transfer_fns resolve_transfers(void) {
    if (jtag_gen_ready_transfers(jtag_gen_wire())) {
        return (struct jtag_transfer_fns){jgen_IR, jgen_Transfer};
    }
    return (struct jtag_transfer_fns){refused_IR, refused_Transfer};
}
#else
/** Which of the copies a call goes to: JTAG, OScan1 paced or not, or the TAP-tracked one.
 *
 * The OScan1 choice is made here rather than inside the frame because making it there costs a
 * cycle in the paced path, which is enough to take a 10 MHz request past the clock the host asked
 * for. Once per call it costs nothing measurable.
 */
#define CJTAG_PICK(name) \
    (cJTAG_Format() != CJTAG_OSCAN1 ? cjtagt_##name : DAP_Data.fast_clock ? cjtagf_##name : cjtag_##name)

void JTAG_Sequence(uint32_t info, const uint8_t* tdi, uint8_t* tdo) {
    if (cJTAG_Active()) {
        CJTAG_PICK(Sequence)(info, tdi, tdo);
    } else {
        jtag4_Sequence(info, tdi, tdo);
    }
}

void JTAG_IR(uint32_t ir) {
    if (cJTAG_Active()) {
        CJTAG_PICK(IR)(ir);
    } else {
        jtag4_IR(ir);
    }
}

uint32_t JTAG_ReadIDCode(void) { return cJTAG_Active() ? CJTAG_PICK(ReadIDCode)() : jtag4_ReadIDCode(); }

void JTAG_WriteAbort(uint32_t data) {
    if (cJTAG_Active()) {
        CJTAG_PICK(WriteAbort)(data);
    } else {
        jtag4_WriteAbort(data);
    }
}

uint8_t JTAG_Transfer(uint32_t request, uint32_t* data) {
    return cJTAG_Active() ? CJTAG_PICK(Transfer)(request, data) : jtag4_Transfer(request, data);
}

struct jtag_transfer_fns {
    void (*ir)(uint32_t ir);
    uint8_t (*transfer)(uint32_t request, uint32_t* data);
};

static struct jtag_transfer_fns resolve_transfers(void) {
    if (cJTAG_Active()) {
        return (struct jtag_transfer_fns){CJTAG_PICK(IR), CJTAG_PICK(Transfer)};
    }
    return (struct jtag_transfer_fns){jtag4_IR, jtag4_Transfer};
}
#endif

#include "jtag_transfer.h"
