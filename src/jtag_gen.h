/** The JTAG wire paths, four-wire and two-pin (IEEE 1149.7 OScan1), generated at run time for the
 * clock the host asked for.
 *
 * src/jtag_gen.c writes them into SRAMX with every edge and sample placed to the core cycle, as
 * src/swd_gen.c does for SWD. src/jtag_gen_dp.c builds ARM's JTAG entry points on them.
 */
#ifndef JTAG_GEN_H
#define JTAG_GEN_H

#include <stdbool.h>
#include <stdint.h>

/** Where the generated code lives: the lower half of SRAMX, which it shares with src/swd_gen.c's
 * (see gen_owner). tools/gen_host builds the generator elsewhere and puts it at an address of its
 * own. */
#ifndef JTAG_GEN_BASE
#define JTAG_GEN_BASE 0x04000000U
#endif
#define JTAG_GEN_SIZE 0x4000U
/** Unrolled transfers longer than this loop their data bits instead, where a loop fits. A loop
 * costs four-wire blocks several percent at 25 and 50 MHz on the bench, and saves up to 6 KB
 * between 11 and 22 MHz. */
#define JTAG_GEN_UNROLLED_MAX 0x3000U

/** A two-pin packet the probe drives is a falling edge, a data store and a rising edge. A
 * four-wire clock is five instructions and runs at no less than five cycles whatever the period. */
#define JTAG_GEN_PERIOD_MIN 3U
/** Longer periods leave the cycle counts of one frame near 32 bits; ARM's path runs them. */
#define JTAG_GEN_PERIOD_MAX (1U << 24)
/** Cycles from the falling edge that launches TDO to the first load that sees it: on two pins the
 * fall that starts the TDO packet, on four wires the clock's own fall. On a CC1312R1 two pins
 * measured 8 after either reset; the Cortex-M DAP behind its ICEPick sends four-wire TDO at 7
 * after a pin reset and 11 after a system reset (CJTAGGEN.md), so four wires default to 10, which
 * read both on the bench. The probe measures from these at every DAP_JTAG_Configure where they
 * leave too little margin (src/jtag_sweep.h). */
#define JTAG_GEN_LATENCY_DEFAULT 8U
#define JTAG_GEN_FOUR_WIRE_LATENCY_DEFAULT 10U
/** A period at which a build for the highest latency the generator takes, 64, samples later
 * than that on both wires, with `earliest` too, and so reads right for every latency
 * (tools/jtag_latency_test.c). */
#define JTAG_GEN_SAFE_PERIOD 72U
/** The most frames one call clocks. */
#define JTAG_GEN_FRAMES_MAX 32U

#define JTAG_GEN_FOUR_WIRE 0U
#define JTAG_GEN_TWO_PIN 1U

struct jtag_gen_config {
    uint32_t wire;     // JTAG_GEN_FOUR_WIRE or JTAG_GEN_TWO_PIN
    uint32_t period;   // core cycles per TCK clock or TCKC packet
    uint32_t latency;  // see JTAG_GEN_LATENCY_DEFAULT
    uint32_t before;   // the transfers' bypass bits before the DAP: its index in the chain
    uint32_t after;    // and after it
    uint32_t idle;     // Run-Test/Idle clocks after each transfer
    uint32_t earliest; // 1: every sample as early as the latency allows, to measure it; scans only
};

/** Clocks `count` JTAG clocks, 1 to JTAG_GEN_FRAMES_MAX, with TMS held at `tms` (0 or 1) and TDI
 * from `tdi`, least significant bit first. Returns TDO the same way.
 *
 * Two-pin: entered and left with TCKC low and TMSC driven, which is what cJTAG_Enter leaves and
 * every frame ends with. Four-wire: entered and left with TCK high, as ARM's clocks leave it. */
typedef uint32_t (*jtag_gen_scan_fn)(uint32_t tdi, uint32_t count, uint32_t tms);

/** Runs `count` transfers of one request back to back, each as ARM's JTAG_Transfer clocks it, and
 * stops at the first that does not acknowledge OK. `request` is DAP's request byte; `words` is read
 * into or written from, four bytes a transfer, unaligned allowed; `tdi` is the level ARM's TDI holds
 * as the first transfer starts. Returns the acknowledge in the low word and, in the high word, the
 * transfers not completed, the one that stopped it included. ARM's TDI is high afterwards. */
typedef uint64_t (*jtag_gen_xfer_fn)(uint32_t request, uint8_t* words, uint32_t count, uint32_t tdi);

/** Generates for `config` into SRAMX: the scan function, then the read and write transfers if they
 * fit, NULL where they do not. With `read` and `write` NULL, the scan function alone. False if the
 * configuration is out of range or the scan function does not fit, in which case nothing generated
 * earlier is usable either. */
bool jtag_gen_build(const struct jtag_gen_config* config, jtag_gen_scan_fn* scan, jtag_gen_xfer_fn* read,
                    jtag_gen_xfer_fn* write);

/** The clock the host last asked for with DAP_SWJ_Clock, in Hz. */
extern uint32_t jtag_gen_clock_hz;
/** The latency each wire is generated for, indexed by JTAG_GEN_FOUR_WIRE and JTAG_GEN_TWO_PIN. */
extern uint32_t jtag_gen_latency[2];

extern jtag_gen_scan_fn jtag_gen_scan;
/** NULL when they did not fit: ARM's transfers then run as scans. */
extern jtag_gen_xfer_fn jtag_gen_read;
extern jtag_gen_xfer_fn jtag_gen_write;

/** How many configurations were generated, and how many could not be and ran ARM's path. */
extern uint32_t jtag_gen_builds;
extern uint32_t jtag_gen_failures;

/** Generates for `wire` at the current clock, latency, chain position and idle cycles if any
 * changed since the last call. Only one wire's code exists at a time. False when it cannot be
 * generated: the caller runs ARM's path instead. */
bool jtag_gen_ready(uint32_t wire);

/** jtag_gen_ready, and on two pins only while the frames are OScan1, which is all the generated
 * code frames. In src/cjtag_dp.c. */
bool jtag_gen_usable(uint32_t wire);

/** What jtag_gen_ready generates `wire` for now, learned idle clocks included. */
struct jtag_gen_config jtag_gen_inputs(uint32_t wire);

/** Run-Test/Idle clocks the generated transfers add to the host's after each transfer, learned from
 * the target's WAITs: one more after each command that met one, up to JTAG_GEN_PACED_MAX, and none
 * again once the host changes the clock or the configuration. A target that answers WAIT to a
 * transfer close behind the last one stops doing so, and one that never does sees ARM's wire. */
extern uint32_t jtag_gen_paced;
#define JTAG_GEN_PACED_MAX 32U
/** Whether WAITs teach jtag_gen_paced. Off, the wire is ARM's exactly. */
extern bool jtag_gen_pacing;
/** A generated transfer was answered WAIT. */
void jtag_gen_waited(void);
/** Between commands: one more idle clock if a WAIT came since the last call, generated now. */
void jtag_gen_learn(uint32_t wire);

/** What the last build produced, for a host to read back. */
struct jtag_gen_info {
    uint32_t period;     // cycles a clock (four-wire) or a packet the probe drives (two-pin)
    uint32_t frame;      // cycles a JTAG clock takes in a run
    uint32_t code_bytes; // out of JTAG_GEN_SIZE
    bool masked;         // runs keep interrupts masked throughout
    bool ok;             // the last configuration generated
    bool transfers;      // and its transfers too
    bool looped;         // with their data bits in a loop, where unrolled they did not fit
    // The real latencies the scans, the read transfers and the write transfers read right for:
    // every one up to these, the fewest cycles any of their samples comes after the fall that
    // launched its bit. The other end of the window is taken as fixed: on four wires TDO holds at
    // least FOUR_WIRE_HOLD cycles past the next fall, which no sample passes, and on two pins
    // every sample comes before it.
    int32_t scan_last;
    int32_t read_last;
    int32_t write_last;
    // Every latency below this one generates the same code in this configuration, or 0 when the
    // latency decided where something went.
    uint32_t same_below;
};

extern struct jtag_gen_info jtag_gen_info;

#endif
