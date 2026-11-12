/** The SWD read latency, measured on the wire: src/latency.h's search, run with DPIDR reads. */
#ifndef SWD_SWEEP_H
#define SWD_SWEEP_H

#include <stdbool.h>
#include <stdint.h>

#include "latency.h"
#include "swd_connect.h"

struct swd_sweep_result {
    struct latency_result result;
    uint32_t cycles;  // the whole sweep, the final generation included
    uint32_t runs;    // sweeps since the probe started
    uint32_t rescues; // connections whose DPIDR read failed and read right once measured
};

/** The last sweep's, for a host to read back. */
extern struct swd_sweep_result swd_sweep_last;

/** Whether the probe measures the latency on its own. A host that sets a latency clears it. */
extern bool swd_sweep_automatic;

/** Watches DAP_SWJ_Sequence for a line reset and DAP_SWJ_Clock for a new clock, and forgets what it
 * measured at DAP_Connect. Every command ARM's code serves passes through here first. */
void swd_sweep_watch(const uint8_t* request);

/** What the host's DAP_SWJ_Sequence bits have done. Read on every DAP_Transfer, from SRAM. */
extern struct swd_connect swd_sweep_line;

/** A DAP_SWJ_Clock that changed the clock since the last DAP_Transfer. */
extern bool swd_sweep_reclocked;

/** Says whether a DAP_Transfer has to go to swd_sweep_connect first: the first since a line reset,
 * or since the host changed the clock. */
static inline bool swd_sweep_pending(void) { return swd_sweep_line.reset || swd_sweep_reclocked; }

/** The first DAP_Transfer after a line reset, run here in place of the usual route; returns what
 * DAP_ProcessCommand does. If it starts with a DPIDR read, which is how a host connects, and the
 * probe measures on its own: first uses what it measured for the same target since the host's
 * DAP_Connect, or measures where the generated reads leave less than two cycles of margin either
 * side for the latency in use, or, on a board that can capture energy, wherever a capture changes
 * the generated reads (gen_stall). Then, if the DPIDR read failed and nothing measured yet,
 * measures and runs the command again when that changed the latency. ARM's transfer loop stops at
 * the first transfer that does not answer OK, so the command ran nothing but that read. */
uint32_t swd_sweep_connect(const uint8_t* request, uint8_t* response);

/** Measures the latency at the current clock and DAP_Data settings, uses what it found
 * (latency_result.latency, when not 0) and generates for it. A running energy capture is held for
 * the measurement (energy_monitor_hold).
 *
 * Each candidate latency is generated and runs DPIDR reads, the one read every SW-DP has to
 * answer OK whatever its state (ADIv5 B4). They are generated with DAP_Data's data phase on, so
 * a read whose acknowledge the probe misreads still clocks to its end and the target never loses
 * step. The caller makes sure a target is selected to answer: on a multidrop wire, after
 * TARGETSEL. */
void swd_sweep(void);

#endif
