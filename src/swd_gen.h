/** The SWD transfer paths, generated at run time for the clock the host asked for.
 *
 * src/swd_gen.c writes two functions into SRAMX, one for reads and one for writes, with every
 * store and every sample placed to the core cycle by the timing model in TIMING.md.
 */
#ifndef SWD_GEN_H
#define SWD_GEN_H

#include <stdbool.h>
#include <stdint.h>

/** Where the generated code lives: the lower half of SRAMX. src/code_lab.c has the upper half. */
#define SWD_GEN_BASE 0x04000000U
#define SWD_GEN_SIZE 0x4000U

/** A read clock is four instructions: two edges, a sample and the sample's use. */
#define SWD_GEN_PERIOD_MIN 4U
/** Longer periods leave the cycle counts of one transfer near 32 bits; ARM's path runs them. */
#define SWD_GEN_PERIOD_MAX (1U << 24)
/** Cycles from a rising-edge store to the first load that sees the bit it launched. The bench
 * measured 7 on two targets; TIMING.md says why it should be measured per target. */
#define SWD_GEN_LATENCY_DEFAULT 7U

struct swd_gen_config {
    uint32_t period;      // core cycles per SWCLK period
    uint32_t latency;     // see SWD_GEN_LATENCY_DEFAULT
    uint32_t turnaround;  // 1 to 4 clocks
    uint32_t data_phase;  // clock a data phase after WAIT and FAULT
    uint32_t idle_cycles; // clocks after each transfer, with SWDIO low
};

/** Runs `count` transfers of one request back to back, and stops at the first that does not
 * acknowledge OK. `words` is read into or written from, four bytes a transfer, unaligned allowed.
 *
 * Returns the acknowledge in the low word, DAP_TRANSFER_ERROR for a read whose parity failed, and
 * in the high word the transfers not completed, the one that stopped it included. */
typedef uint64_t (*swd_gen_fn)(uint32_t packet, uint8_t* words, uint32_t count);

/** The eight request bits as the generated code clocks them, bit n on clock n. */
static inline uint32_t swd_gen_packet(uint32_t request) {
    const uint32_t bits = request & 0x0FU;
    return 1U | (bits << 1) | ((uint32_t)__builtin_parity(bits) << 5) | (1U << 7);
}

/** Generates both functions for `config` into SRAMX. False if the configuration is out of range
 * or the code does not fit, in which case nothing generated earlier is usable either. */
bool swd_gen_build(const struct swd_gen_config* config, swd_gen_fn* read, swd_gen_fn* write);

/** The clock the host last asked for with DAP_SWJ_Clock, in Hz. */
extern uint32_t swd_gen_clock_hz;
/** The latency transfers are generated for. */
extern uint32_t swd_gen_latency;

extern swd_gen_fn swd_gen_read;
extern swd_gen_fn swd_gen_write;

/** How many configurations were generated, and how many could not be and ran ARM's path. */
extern uint32_t swd_gen_builds;
extern uint32_t swd_gen_failures;

/** Generates for the current clock, latency and DAP_Data settings if they changed since the
 * last call. False when they cannot be generated: the caller runs ARM's transfer instead. */
bool swd_gen_ready(void);

#endif
