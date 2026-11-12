/** The JTAG and cJTAG read latency, measured on the wire: src/latency.h's search, run with
 * IDCODE reads. */
#ifndef JTAG_SWEEP_H
#define JTAG_SWEEP_H

#include <stdbool.h>
#include <stdint.h>

#include "latency.h"

struct jtag_sweep_result {
    struct latency_result result;
    uint32_t wire;    // JTAG_GEN_FOUR_WIRE, _TWO_PIN, _OSCAN2 or _OSCAN5
    uint32_t cycles;  // the whole sweep, the final generation included
    uint32_t runs;    // sweeps since the probe started
    uint32_t rescues; // connections whose first transfer failed and ran right once measured
};

/** The last sweep's, for a host to read back. */
extern struct jtag_sweep_result jtag_sweep_last;

/** Whether the probe measures the latency on its own. A host that sets a latency clears it. */
extern bool jtag_sweep_automatic;

/** Watches for DAP_JTAG_Configure, and DAP_SWJ_Clock for a new clock. Every command ARM's code
 * serves passes through here first. */
void jtag_sweep_watch(const uint8_t* request);

/** A DAP_JTAG_Configure or a new clock since the host's last DAP_Transfer or the last measurement.
 * Read on every DAP_Transfer, from SRAM. */
extern bool jtag_sweep_configured;

/** Says whether a DAP_Transfer on the JTAG port has to go to jtag_sweep_connect first. A host
 * configures the chain when it connects, and probe-rs again after it resets the target. */
static inline bool jtag_sweep_pending(void) { return jtag_sweep_configured; }

/** The first DAP_Transfer after DAP_JTAG_Configure, run here in place of ARM's; returns what
 * DAP_ProcessCommand does. If the probe measures on its own: it goes back to the wire's default
 * latency, and measures where the code generated for it leaves less than two cycles for a later
 * TDO. Then, if the command's first transfer failed and nothing measured yet, measures and runs
 * the command again when that changed the latency. ARM's transfer loop stops at the first transfer
 * that does not answer OK, so the command ran nothing but that one, whose acknowledge the probe did
 * not take as OK.
 *
 * Nothing measured at an earlier configuration is reused. A CC13xx's JTAG TDO comes four
 * cycles later after a system reset, which probe-rs follows with DAP_JTAG_Configure. A transfer run
 * at a stale latency need not fail where it can be seen: samples between two bits can read an OK
 * acknowledge, and a misread one leaves Shift-DR early, so that the Update-DR after it makes an
 * access of its own. */
uint32_t jtag_sweep_connect(const uint8_t* request, uint8_t* response);

/** Measures the latency of the wire in use at the current clock and chain position, uses what it
 * found (latency_result.latency, when not 0; a cycle more with cJTAG) and generates for it. A
 * running energy capture is held for the measurement (energy_monitor_hold).
 *
 * Each candidate latency is generated, the scan function alone, and reads the IDCODE of the TAP
 * at DAP_Data's chain index. An IDCODE scan changes nothing in the TAP, however many bits it
 * shifts. A DPACC read would not do: one whose OK acknowledge the probe misread leaves Shift-DR
 * early, and its Update-DR makes whatever access the bits captured from the last read spell. It
 * leaves that TAP's IR at IDCODE, which the next DAP_Transfer sets again. */
void jtag_sweep(void);

#endif
