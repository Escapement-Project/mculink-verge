/** The JTAG entry points, routed to four-wire or OScan1.
 *
 * ARM's JTAG_DP.c is compiled with its five entry points renamed to `jtag4_*` (see CMakeLists), so
 * its source stays unpatched and the four-wire path keeps no per-bit branch. This file owns the
 * real names and picks a path once per call.
 *
 * Only the wire framing differs. The TAP state walking, the scan lengths and the transfer retry
 * logic are the same for two-pin and four-pin, so nothing here reimplements them.
 */
#include "DAP_config.h"
#include "DAP.h"
#include "cjtag.h"

extern void     jtag4_Sequence   (uint32_t info, const uint8_t *tdi, uint8_t *tdo);
extern void     jtag4_IR         (uint32_t ir);
extern uint32_t jtag4_ReadIDCode (void);
extern void     jtag4_WriteAbort (uint32_t data);
extern uint8_t  jtag4_Transfer   (uint32_t request, uint32_t *data);

/** OScan1 form of JTAG_Sequence: same bit accounting, one frame per clock. */
static void cjtag_sequence(uint32_t info, const uint8_t *tdi, uint8_t *tdo) {
  uint32_t n = info & JTAG_SEQUENCE_TCK;
  if (n == 0U) {
    n = 64U;
  }

  const uint32_t tms = (info & JTAG_SEQUENCE_TMS) ? 1U : 0U;

  while (n != 0U) {
    uint32_t in = *tdi++;
    uint32_t out = 0U;
    uint32_t k;

    for (k = 8U; (k != 0U) && (n != 0U); k--, n--) {
      uint32_t bit = cJTAG_Frame(tms, in & 1U);
      in >>= 1;
      out >>= 1;
      out |= bit << 7;
    }
    // A sequence need not end on a byte boundary; the captured bits sit in the high end of the
    // byte until the unused positions are shifted out.
    out >>= k;

    if ((info & JTAG_SEQUENCE_TDO) != 0U) {
      *tdo++ = (uint8_t)out;
    }
  }
}

void JTAG_Sequence(uint32_t info, const uint8_t *tdi, uint8_t *tdo) {
  if (cJTAG_Active()) {
    cjtag_sequence(info, tdi, tdo);
  } else {
    jtag4_Sequence(info, tdi, tdo);
  }
}

// The remaining four entry points have no OScan1 form yet. Falling through to the four-wire
// versions would drive TDI and TCK at a target that has none of either and would corrupt the
// two-pin wire, so while OScan1 is active they refuse instead. DAP_JTAG_Sequence is enough to
// scan a chain and read an IDCODE, which is the first thing to get working on the bench.

void JTAG_IR(uint32_t ir) {
  if (!cJTAG_Active()) {
    jtag4_IR(ir);
  }
}

uint32_t JTAG_ReadIDCode(void) {
  return cJTAG_Active() ? 0U : jtag4_ReadIDCode();
}

void JTAG_WriteAbort(uint32_t data) {
  if (!cJTAG_Active()) {
    jtag4_WriteAbort(data);
  }
}

uint8_t JTAG_Transfer(uint32_t request, uint32_t *data) {
  if (cJTAG_Active()) {
    return DAP_TRANSFER_ERROR;
  }
  return jtag4_Transfer(request, data);
}
