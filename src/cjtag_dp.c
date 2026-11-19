/** The JTAG entry points ARM's DAP.c calls, routed to the generated code for the wire in use.
 *
 * Product images only. The ARM reference build compiles ARM's JTAG_DP.c, with the cJTAG of
 * external/cmsis-dap-cjtag.patch, under these names instead.
 */
// DAP_config.h first: DAP.h reads the DAP_SWD and DAP_JTAG settings it defines, so a sorted
// include block that puts DAP.h ahead of it will not compile.
#include "DAP_config.h"

#include "DAP.h"
#include "cjtag.h"
#include "hot.h"
#include "jtag_dp.h"
#include "jtag_gen.h"

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

#include "jtag_transfer.h"
