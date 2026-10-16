/** ARM's JTAG implementation, compiled a second time to drive OScan1.
 *
 * The alternative was reimplementing the five entry points. That would duplicate the TAP walking,
 * the ir_before/ir_length accounting and the transfer retry logic -- about 250 lines whose only
 * job is to be identical to ARM's, and which would drift from it silently. Only the wire framing
 * differs between two-pin and four-pin, so only the wire framing is replaced here.
 *
 * **The contract this rests on.** JTAG_DP.c expresses every clock through four macros that expand,
 * in this order, to: PIN_TDI_OUT (sometimes), PIN_TCK_CLR, PIN_DELAY, PIN_TDO_IN (sometimes),
 * PIN_TCK_SET, PIN_DELAY. Shadowing those names turns a four-wire clock into one OScan1 frame:
 * the pin calls stage TMS and TDI, PIN_TCK_CLR opens a frame, and whichever of PIN_TDO_IN or
 * PIN_TCK_SET comes first emits it. The submodule is pinned at a release, so that order cannot
 * change without someone bumping the tag and reading this.
 */
#include <stdbool.h>
#include <stdint.h>

// DAP_config.h first: DAP.h reads the DAP_SWD and DAP_JTAG settings it defines, so a sorted
// include block that puts DAP.h ahead of it will not compile.
#include "DAP_config.h"

#include "DAP.h"
#include "cjtag.h"
#include "jtag_dp.h"

static uint32_t staged_tms;
static uint32_t staged_tdi;
static uint32_t captured_tdo;
static bool frame_open;

static inline void cjtag_emit(void) {
    if (frame_open) {
        captured_tdo = cJTAG_Frame(staged_tms, staged_tdi);
        frame_open = false;
    }
}

static inline uint32_t cjtag_capture(void) {
    cjtag_emit();
    return captured_tdo;
}

// The names JTAG_DP.c's five entry points take in this copy. jtag_dp.h, included above, has
// already declared them, which is what satisfies -Wmissing-prototypes. The four static helpers
// are renamed too, so a map file and a debugger can tell the two copies apart.
#define JTAG_Sequence cjtag_Sequence
#define JTAG_IR cjtag_IR
#define JTAG_ReadIDCode cjtag_ReadIDCode
#define JTAG_WriteAbort cjtag_WriteAbort
#define JTAG_Transfer cjtag_Transfer
#define JTAG_TransferFast cjtag_TransferFast
#define JTAG_TransferSlow cjtag_TransferSlow
#define JTAG_IR_Fast cjtag_IR_Fast
#define JTAG_IR_Slow cjtag_IR_Slow

// The wire layer. These shadow the inline functions from DAP_config.h for this file only.
#undef PIN_SWDIO_TMS_SET
#undef PIN_SWDIO_TMS_CLR
#undef PIN_TDI_OUT
#undef PIN_TDO_IN
#undef PIN_SWCLK_TCK_SET
#undef PIN_SWCLK_TCK_CLR
#define PIN_SWDIO_TMS_SET() (staged_tms = 1U)
#define PIN_SWDIO_TMS_CLR() (staged_tms = 0U)
#define PIN_TDI_OUT(bit) (staged_tdi = (uint32_t)(bit) & 1U)
#define PIN_TDO_IN() cjtag_capture()
#define PIN_SWCLK_TCK_CLR() (frame_open = true)
#define PIN_SWCLK_TCK_SET() cjtag_emit()

#include "JTAG_DP.c"
