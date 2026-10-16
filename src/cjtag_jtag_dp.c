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

// Which frame this copy drives, and what its entry points are called. src/cjtag_jtag_dp_fast.c
// includes this file with both overridden, so the paced and delay-free copies share one source.
#ifndef CJTAG_FRAME
#define CJTAG_FRAME cJTAG_FrameSlow
#define CJTAG_NAME(name) cjtag_##name
#endif

static uint32_t staged_tms;
static uint32_t staged_tdi;
static uint32_t captured_tdo;
static bool frame_open;

static inline void cjtag_emit(void) {
    if (frame_open) {
        captured_tdo = CJTAG_FRAME(staged_tms, staged_tdi);
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
#define JTAG_Sequence CJTAG_NAME(Sequence)
#define JTAG_IR CJTAG_NAME(IR)
#define JTAG_ReadIDCode CJTAG_NAME(ReadIDCode)
#define JTAG_WriteAbort CJTAG_NAME(WriteAbort)
#define JTAG_Transfer CJTAG_NAME(Transfer)
#define JTAG_TransferFast CJTAG_NAME(TransferFast)
#define JTAG_TransferSlow CJTAG_NAME(TransferSlow)
#define JTAG_IR_Fast CJTAG_NAME(IR_Fast)
#define JTAG_IR_Slow CJTAG_NAME(IR_Slow)

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

// The frame does its own pacing, so the delays JTAG_DP.c wraps around these no-op clock calls are
// dead time: eight per frame where six are wanted. Suppressing them is worth 20% of a two-pin bit
// at a paced clock, and nothing at all on the delay-free path, where they are already empty.
#undef PIN_DELAY_SLOW
#undef PIN_DELAY_FAST
#define PIN_DELAY_SLOW(delay) ((void)(delay))
#define PIN_DELAY_FAST() ((void)0)

#include "JTAG_DP.c"

// The same block loop the four-wire path uses, expanded next to this copy's transfer so the
// registers and the GPIO addresses are paid for once a block rather than once a word. `flatten`
// is what makes that actually happen here: this copy's transfer is large enough that the compiler
// declines to inline it, and the block loop ends up calling it once a word. It costs flash, and
// the flash was judged worth it. OPTIMISATIONS.md has the numbers.
#define JTAG_BLOCK_ATTRS __attribute__((flatten)) HOT_PATH
#include "jtag_block.h"

// clang-format off: each of these expands to a function definition.
JTAG_BLOCK_FUNCTION(CJTAG_NAME(BlockImpl), CJTAG_NAME(Transfer), CJTAG_NAME(IR))
// clang-format on

/** The same return shape DAP.c's own DAP_TransferBlock produces. */
uint32_t CJTAG_NAME(TransferBlock)(const uint8_t* request, uint8_t* response) {
    uint32_t num = CJTAG_NAME(BlockImpl)(request, response);

    if ((*(request + 3) & DAP_TRANSFER_RnW) != 0U) {
        num |= 4U << 16;
    } else {
        num |= (4U + (((uint32_t)(*(request + 1)) | (uint32_t)(*(request + 2) << 8)) * 4)) << 16;
    }
    return num;
}
