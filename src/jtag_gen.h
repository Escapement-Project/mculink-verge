/** The JTAG and cJTAG wire paths (cJTAG in IEEE 1149.7 OScan1, OScan2, OScan5 and OScan6),
 * generated at runtime for the clock the host asked for.
 *
 * src/jtag_gen.c writes them into SRAMX with every edge and sample placed to the core cycle, as
 * src/swd_gen.c does for SWD. src/jtag_gen_dp.c builds ARM's JTAG entry points on them.
 */
#ifndef JTAG_GEN_H
#define JTAG_GEN_H

#include <stdbool.h>
#include <stdint.h>

/** Where the generated code lives: SRAMX, whose lower half it shares with src/swd_gen.c's (see
 * gen_owner). cJTAG's scans can take most of that half, OScan6's at every clock, so the transfers
 * run on into the upper one; a code lab build keeps the upper half for the lab's arena
 * (src/code_lab.c). tools/gen_host builds the generator elsewhere and puts it at an address of its
 * own. */
#ifndef JTAG_GEN_BASE
#define JTAG_GEN_BASE 0x04000000U
#endif
#ifdef CODE_LAB
#define JTAG_GEN_SIZE 0x4000U
#else
#define JTAG_GEN_SIZE 0x8000U
#endif
/** Unrolled transfers have to end this far into the region, the scans before them included. Past
 * it they loop their data bits instead, where a loop fits, which costs a few cycles a word. */
#define JTAG_GEN_UNROLLED_MAX 0x3000U

/** A cJTAG packet the probe drives is a falling edge, a data store and a rising edge. A
 * JTAG clock is five instructions and runs at no less than five cycles whatever the period. */
#define JTAG_GEN_PERIOD_MIN 3U
/** Longer periods leave the cycle counts of one frame near 32 bits. DAP_SWJ_Clock refuses the
 * clocks that would need them (src/dap_dispatch.c). */
#define JTAG_GEN_PERIOD_MAX (1U << 24)
/** Cycles from the falling edge that launches TDO to the first load that sees it: with cJTAG the
 * fall that starts the TDO packet, with JTAG the clock's own fall. On a CC1312R1 cJTAG
 * measured 8 after either reset; the Cortex-M DAP behind its ICEPick sends JTAG's TDO at 7
 * after a pin reset and 11 after a system reset (CJTAG.md), so JTAG defaults to 10, which
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
#define JTAG_GEN_TWO_PIN 1U // OScan1
#define JTAG_GEN_OSCAN2 2U  // cJTAG in OScan2: one TMS packet a clock outside Shift
#define JTAG_GEN_OSCAN5 3U  // cJTAG in OScan5: nTDI and TDO in Shift, leaving it by an escape
#define JTAG_GEN_OSCAN6 4U  // cJTAG in OScan6: OScan2 outside Shift, OScan5 in it, a PAD on entering
#define JTAG_GEN_WIRES 5U
/** The index of jtag_gen_latency a wire uses: every cJTAG format reads the same TDO packet. */
#define JTAG_GEN_PINS(wire) ((wire) == JTAG_GEN_FOUR_WIRE ? JTAG_GEN_FOUR_WIRE : JTAG_GEN_TWO_PIN)

struct jtag_gen_config {
    uint32_t wire;     // JTAG_GEN_FOUR_WIRE, _TWO_PIN, _OSCAN2 or _OSCAN5
    uint32_t period;   // core cycles per TCK clock or TCKC packet
    uint32_t latency;  // see JTAG_GEN_LATENCY_DEFAULT
    uint32_t before;   // the transfers' bypass bits before the DAP: its index in the chain
    uint32_t after;    // and after it
    uint32_t idle;     // Run-Test/Idle clocks after each transfer
    uint32_t earliest; // 1: every sample as early as the latency allows and before the fall that ends
                       // its bit, to measure it; scans only
    uint32_t stall;    // see gen_stall; JTAG only, since cJTAG samples precede their window's end
};

/** The longest stall a build takes. A sample's window past the next fall ends the stall short of
 * TDO's hold, less a cycle's margin; with more, on the shortest clock, 5 cycles, it would end
 * before the clock began. */
#define JTAG_GEN_STALL_MAX 4U

/** Clocks `count` JTAG clocks, 1 to JTAG_GEN_FRAMES_MAX, with TMS held at `tms` (0 or 1) and TDI
 * from `tdi`, least significant bit first. Returns TDO the same way.
 *
 * cJTAG: entered and left with TCKC low and TMSC driven, which is what cJTAG_Enter leaves and
 * every frame ends with. JTAG: entered and left with TCK high, as ARM's clocks leave it. */
typedef uint32_t (*jtag_gen_scan_fn)(uint32_t tdi, uint32_t count, uint32_t tms);

/** OScan2 and OScan6: `count` frames of one TMS packet each, for clocks outside Shift-DR and Shift-IR,
 * with TMS held at `tms`. `tdi` goes nowhere, and the frames bring nothing back, so this returns
 * all ones, what a capture there reads in OScan1. Entered and left as the scan function is. */
extern jtag_gen_scan_fn jtag_gen_tms;

/** OScan5 and OScan6. In OScan5 the scan function frames OScan1's three packets, for clocks outside
 * Shift-DR and Shift-IR; in OScan6 jtag_gen_tms does. jtag_gen_shift frames those in them, nTDI
 * and TDO with TMS 0, and jtag_gen_eot the one that leaves them, TMS 1 by an EOT escape on its nTDI
 * packet. OScan6's first frame after entering Shift has a PAD packet: the scan function frames it,
 * or jtag_gen_eot_pad where it also leaves Shift, either sending the PAD from its TMS argument. jtag_gen_eot and jtag_gen_eot_pad are
 * one block: `count` above 1 enters them before their start, in the code before. All return TDO as
 * the scan function does, and are entered and left as it is. */
extern jtag_gen_scan_fn jtag_gen_shift;
extern jtag_gen_scan_fn jtag_gen_eot;
extern jtag_gen_scan_fn jtag_gen_eot_pad;

/** Runs `count` transfers of one request back to back, each as ARM's JTAG_Transfer clocks it, and
 * stops at the first that does not acknowledge OK. `request` is DAP's request byte; `words` is read
 * into or written from, four bytes a transfer, unaligned allowed; `tdi` is the level ARM's TDI holds
 * as the first transfer starts. Returns the acknowledge in the low word and, in the high word, the
 * transfers not completed, the one that stopped it included. ARM's TDI is high afterwards. */
typedef uint64_t (*jtag_gen_xfer_fn)(uint32_t request, uint8_t* words, uint32_t count, uint32_t tdi);

/** Generates for `config` into SRAMX: the scan function, OScan2's and OScan6's TMS packets
 * (jtag_gen_tms), OScan5's and OScan6's Shift frames (jtag_gen_shift, jtag_gen_eot,
 * jtag_gen_eot_pad), then the read and write transfers if they fit, nullptr where they do not. With
 * `read` and `write` nullptr, no transfers. False if the
 * configuration is out of range or the scan function does not fit, in which case nothing generated
 * earlier is usable either. */
bool jtag_gen_build(const struct jtag_gen_config* config, jtag_gen_scan_fn* scan, jtag_gen_xfer_fn* read,
                    jtag_gen_xfer_fn* write);

/** The clock the host last asked for with DAP_SWJ_Clock, in Hz. */
extern uint32_t jtag_gen_clock_hz;
/** The latency each wire is generated for, indexed by JTAG_GEN_PINS(wire). */
extern uint32_t jtag_gen_latency[2];

extern jtag_gen_scan_fn jtag_gen_scan;
/** nullptr when they did not fit: ARM's transfer logic then runs over the generated scans. */
extern jtag_gen_xfer_fn jtag_gen_read;
extern jtag_gen_xfer_fn jtag_gen_write;

/** How many configurations were generated, how many could not be and refused the wire, and how many
 * were copied back from a kept build with transfers instead. */
extern uint32_t jtag_gen_builds;
extern uint32_t jtag_gen_failures;
extern uint32_t jtag_gen_restores;

/** The last JTAG_GEN_LOG_SIZE generations, so a host can say what each one was for. Entry n is
 * generation n (jtag_gen_builds, jtag_gen_failures and jtag_gen_restores before it) modulo the
 * size. The sweep's candidates are not generations; the one after it shows JTAG_GEN_CHANGED_OWNER. */
#define JTAG_GEN_LOG_SIZE 16U
#define JTAG_GEN_CHANGED_FIRST 0x01U     // the first since the probe started
#define JTAG_GEN_CHANGED_OWNER 0x02U     // SRAMX held other code: SWD's, or a sweep's candidate
#define JTAG_GEN_CHANGED_WIRE 0x04U
#define JTAG_GEN_CHANGED_PERIOD 0x08U
#define JTAG_GEN_CHANGED_LATENCY 0x10U
#define JTAG_GEN_CHANGED_CHAIN 0x20U     // the DAP's position in the chain
#define JTAG_GEN_CHANGED_IDLE 0x40U      // the host's idle clocks plus the learned ones
#define JTAG_GEN_CHANGED_TRANSFERS 0x80U // the last build was scans only
#define JTAG_GEN_LOG_TRANSFERS 0x01U     // this build has the transfers
#define JTAG_GEN_LOG_LEARNED 0x02U       // after a WAIT taught one more idle clock
#define JTAG_GEN_LOG_OK 0x04U
#define JTAG_GEN_LOG_RESTORED 0x08U // copied back from a kept build with transfers, not built
struct jtag_gen_log_entry {
    uint8_t command; // the DAP command it was generated in, jtag_gen_command
    uint8_t wire;
    uint8_t changed; // JTAG_GEN_CHANGED_*, against the last generation
    uint8_t flags;   // JTAG_GEN_LOG_*
    uint8_t latency;
    uint8_t before;
    uint8_t after;
    uint8_t idle;  // learned clocks included
    uint8_t paced; // of which learned
    uint16_t period;
    uint32_t at;     // TIMESTAMP_GET, the core clock's cycles, when it started
    uint32_t cycles; // how long it took
};
extern struct jtag_gen_log_entry jtag_gen_log[JTAG_GEN_LOG_SIZE];
/** The id of the DAP command being processed, set by DAP_ProcessCommand for the log. */
extern uint8_t jtag_gen_command;

/** Generates for `wire` at the current clock, latency, chain position and idle cycles if any
 * changed since the last call. Only one wire's code exists at a time. False when it cannot be
 * generated: there is no ARM path behind it, and the caller fails without touching the wire. */
bool jtag_gen_ready(uint32_t wire);
/** jtag_gen_ready with the transfers too, where they fit. jtag_gen_ready alone builds the scans, so a
 * framing switch or a new configuration costs no transfer generation until a transfer needs one;
 * a scan-only build leaves jtag_gen_read and jtag_gen_write nullptr, though code kept or restored with
 * its transfers for a scan-only request leaves them set. */
bool jtag_gen_ready_transfers(uint32_t wire);

/** The wire the probe drives now, for jtag_gen_ready. In src/cjtag_dp.c. */
uint32_t jtag_gen_wire(void);

/** What jtag_gen_ready generates `wire` for now, learned idle clocks included. */
struct jtag_gen_config jtag_gen_inputs(uint32_t wire);

/** Run-Test/Idle clocks the generated transfers add to the host's after each transfer, learned from
 * the target's WAITs: one more after each command that met one, up to JTAG_GEN_PACED_MAX. A target
 * that answers WAIT to a transfer close behind the last one stops doing so, and one that never does
 * sees ARM's wire. Each wire keeps what it learned for one configuration, the clock and the chain,
 * and uses it whenever the host comes back to that configuration; any other starts from none. A
 * host that frames OScan1 and a shorter chain on its way to an OScan2 session so learns nothing
 * twice. This is the wire in use's. */
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
    uint32_t period;     // cycles a clock (JTAG) or a packet the probe drives (cJTAG)
    uint32_t frame;      // cycles a JTAG clock takes in a run
    uint32_t code_bytes; // out of JTAG_GEN_SIZE
    bool masked;         // runs keep interrupts masked throughout
    bool ok;             // the last configuration generated
    bool transfers;      // and its transfers too
    bool looped;         // with their data bits in a loop, where unrolled they did not fit
    // JTAG: a sample may come after the fall that ends its bit's window, so a stall (gen_stall)
    // changes the code. cJTAG samples always come before it.
    bool stall_moves;
    // The real latencies the scans, the read transfers and the write transfers read right for,
    // from first to last. The last is the fewest cycles any of their samples comes after the fall
    // that launched its bit. TDO changes as long after every fall, so it holds a bit one cycle
    // less than the latency past the next fall: the first is one more than the most cycles any
    // sample, a stall included, comes after that fall, and 1 when every sample comes before it,
    // as every cJTAG sample does.
    int32_t scan_first;
    int32_t scan_last;
    int32_t read_first;
    int32_t read_last;
    int32_t write_first;
    int32_t write_last;
    // Every latency below this one generates the same code in this configuration, or 0 when the
    // latency decided where something went.
    uint32_t same_below;
};

extern struct jtag_gen_info jtag_gen_info;

#endif
