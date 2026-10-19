/** The SWD read latency, measured on the wire: src/swd_latency.h's search, run with DPIDR reads. */
#ifndef SWD_SWEEP_H
#define SWD_SWEEP_H

#include <stdbool.h>
#include <stdint.h>

#include "swd_connect.h"
#include "swd_latency.h"

struct swd_sweep_result {
    struct swd_latency_result result;
    uint32_t cycles; // the whole sweep, the final generation included
    uint32_t runs;   // sweeps since the probe started
};

/** The last sweep's, for a host to read back. */
extern struct swd_sweep_result swd_sweep_last;

/** Whether the probe measures the latency on its own. A host that sets a latency clears it. */
extern bool swd_sweep_automatic;

/** Watches DAP_SWJ_Sequence for a line reset, and forgets what it measured at DAP_Connect. Every
 * command ARM's code serves passes through here first. */
void swd_sweep_watch(const uint8_t* request);

/** What the host's DAP_SWJ_Sequence bits have done. Read on every DAP_Transfer, from SRAM. */
extern struct swd_connect swd_sweep_line;

/** Says whether a DAP_Transfer has to go to swd_sweep_connect first: the first since a line reset. */
static inline bool swd_sweep_pending(void) { return swd_sweep_line.reset; }

/** The first DAP_Transfer after a line reset. If it starts with a DPIDR read, which is how a host
 * connects, and the probe measures on its own: uses what it measured for the same target since the
 * host's DAP_Connect, or measures, where the generated reads leave less than two cycles of margin
 * either side for the latency in use. */
void swd_sweep_connect(const uint8_t* request);

/** Measures the latency at the current clock and DAP_Data settings, uses what it found
 * (swd_latency_result.latency, when not 0) and generates for it.
 *
 * Each candidate latency is generated and runs DPIDR reads, the one read every SW-DP has to
 * answer OK whatever its state (ADIv5 B4). They are generated with DAP_Data's data phase on, so
 * a read whose acknowledge the probe misreads still clocks to its end and the target never loses
 * step. The caller makes sure a target is selected to answer: on a multidrop wire, after
 * TARGETSEL. */
void swd_sweep(void);

#endif
