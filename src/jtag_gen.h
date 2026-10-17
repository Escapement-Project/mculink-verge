/** The two-pin (IEEE 1149.7 OScan1) wire path, generated at run time for the clock the host asked
 * for.
 *
 * src/jtag_gen.c writes it into SRAMX with every edge and sample placed to the core cycle, as
 * src/swd_gen.c does for SWD. src/jtag_gen_dp.c builds ARM's JTAG entry points on it.
 */
#ifndef JTAG_GEN_H
#define JTAG_GEN_H

#include <stdbool.h>
#include <stdint.h>

/** Where the generated code lives: above the SWD paths, below src/code_lab.c's half.
 * tools/gen_host builds the generator elsewhere and puts it at an address of its own. */
#ifndef JTAG_GEN_BASE
#define JTAG_GEN_BASE 0x04002000U
#endif
#define JTAG_GEN_SIZE 0x2000U

/** A packet the probe drives is a falling edge, a data store and a rising edge. */
#define JTAG_GEN_PERIOD_MIN 3U
/** Longer periods leave the cycle counts of one frame near 32 bits; ARM's path runs them. */
#define JTAG_GEN_PERIOD_MAX (1U << 24)
/** Cycles from the falling edge that starts a TDO packet to the first load that sees TDO. The
 * bench measured 8 on a CC1312R1 at every period from 10 to 30 (CJTAGGEN.md). */
#define JTAG_GEN_LATENCY_DEFAULT 8U
/** The most frames one call clocks. */
#define JTAG_GEN_FRAMES_MAX 32U

struct jtag_gen_config {
    uint32_t period;  // core cycles per TCKC packet
    uint32_t latency; // see JTAG_GEN_LATENCY_DEFAULT
};

/** Clocks `count` frames, 1 to JTAG_GEN_FRAMES_MAX, with TMS held at `tms` (0 or 1) and TDI from
 * `tdi`, least significant bit first. Returns TDO the same way.
 *
 * Entered and left with TCKC low and TMSC driven, which is what cJTAG_Enter leaves and every frame
 * ends with. */
typedef uint32_t (*jtag_gen_scan_fn)(uint32_t tdi, uint32_t count, uint32_t tms);

/** Generates for `config` into SRAMX. False if the configuration is out of range or the code does
 * not fit, in which case nothing generated earlier is usable either. */
bool jtag_gen_build(const struct jtag_gen_config* config, jtag_gen_scan_fn* scan);

/** The clock the host last asked for with DAP_SWJ_Clock, in Hz. */
extern uint32_t jtag_gen_clock_hz;
/** The latency frames are generated for. */
extern uint32_t jtag_gen_latency;

extern jtag_gen_scan_fn jtag_gen_scan;

/** How many configurations were generated, and how many could not be and ran ARM's path. */
extern uint32_t jtag_gen_builds;
extern uint32_t jtag_gen_failures;

/** Generates for the current clock and latency if they changed since the last call. False when
 * they cannot be generated: the caller runs ARM's path instead. */
bool jtag_gen_ready(void);

/** What the last build produced, for a host to read back. */
struct jtag_gen_info {
    uint32_t period;     // cycles a packet the probe drives
    uint32_t frame;      // cycles a frame takes in a run
    uint32_t code_bytes; // out of JTAG_GEN_SIZE
    bool masked;         // runs keep interrupts masked throughout, rather than around each release
    bool ok;             // the last configuration generated
};

extern struct jtag_gen_info jtag_gen_info;

#endif
