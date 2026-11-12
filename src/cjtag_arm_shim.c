/** Test build only (CMake ARM_CJTAG, version "+arm-cjtag"): Verge's cJTAG entry points on ARM's
 * DAP_JTAG_Format and DAP_JTAG_Escape, so the host tools, the emulator checks and Verge's vendor
 * IDs drive the cJTAG proposed for ARM's CMSIS-DAP rather than Verge's own. ARM's JTAG_DP.c frames
 * every JTAG command itself; this only translates the requests. */
// DAP_config.h first: DAP.h reads the DAP_SWD and DAP_JTAG settings it defines, so a sorted
// include block that puts DAP.h ahead of it will not compile.
#include "DAP_config.h"

#include "DAP.h"
#include "cjtag.h"

extern uint32_t arm_DAP_ProcessCommand(const uint8_t* request, uint8_t* response);

static_assert(DAP_CJTAG != 0, "the test build turns ARM's cJTAG on (src/DAP_config.h)");

bool cJTAG_Active(void) { return DAP_Data.jtag_format != DAP_JTAG_FORMAT_JTAG; }

// SCNFMT, 8 + the format.
uint32_t cJTAG_Format(void) { return 8U + DAP_Data.jtag_format; }

bool cJTAG_Select(uint32_t value) {
    uint8_t const request[] = {ID_DAP_JTAG_Format, (uint8_t)value};
    uint8_t response[2] = {0U};
    (void)arm_DAP_ProcessCommand(request, response);
    return response[1] == DAP_OK;
}

void cJTAG_Escape(uint32_t edges, uint32_t level) {
    uint8_t const request[] = {ID_DAP_JTAG_Escape, (uint8_t)edges, (uint8_t)level};
    uint8_t response[2] = {0U};
    (void)arm_DAP_ProcessCommand(request, response);
}

// ARM's DAP_Connect and DAP_Disconnect end the format themselves.
void cJTAG_Deactivate(void) {}
