/** The SWD read latency, measured on the wire. See swd_sweep.h and latency.h. */
#include "swd_sweep.h"

#include "DAP_config.h"

#include "DAP.h"
#include "byte_io.h"
#include "cjtag.h"
#include "dap_shared.h"
#include "energy_monitor.h"
#include "gen.h"
#include "swd_connect.h"
#include "swd_dp.h"
#include "swd_gen.h"

/** DPIDR reads at each candidate. No sample position on the bench ever gave a mix of right and
 * wrong reads, so these are for a target that does. */
#define SWEEP_READS 64U

/** A DP read of address 0, DPIDR. */
#define DPIDR_READ 0x02U

struct swd_sweep_result swd_sweep_last;
bool swd_sweep_automatic = true;

/** Cycles of margin either side below which a connection measures. */
#define CONNECT_MARGIN 2

/** Targets measured since the host's DAP_Connect: a multidrop host switches between them by line
 * reset and TARGETSEL, and measuring again at each switch would cost it every time. */
#define KNOWN_TARGETS 4U

struct known {
    bool multidrop;
    uint32_t targetsel;
    uint32_t latency;
};

struct swd_connect swd_sweep_line;
bool swd_sweep_reclocked;
static struct known known[KNOWN_TARGETS];
static uint32_t known_count;
/** The clock the host last asked for, to tell a change from the same one again. */
static uint32_t asked_hz;

static uint32_t sweep_words[SWEEP_READS];

/** The host's configuration, data phase on. A longer read period than the host's is generated as
 * the write period too, which only slows the packet. */
struct sweep_context {
    struct swd_gen_config config;
    uint32_t read_period;
};

static void try_latency(uint32_t period, uint32_t latency, struct latency_try* out, void* context) {
    const struct sweep_context* c = context;
    struct swd_gen_config config = c->config;
    config.latency = latency;
    if (period != c->read_period) {
        config.period = period;
    }
    swd_gen_fn read;
    swd_gen_fn write;
    if (!swd_gen_build(&config, &read, &write)) {
        return;
    }
    out->generated = true;
    out->first = swd_gen_info.read_first;
    out->last = swd_gen_info.read_last;
    out->same_below = swd_gen_info.masked_from;
    const uint64_t result = read(swd_gen_packet(DPIDR_READ), (uint8_t*)sweep_words, SWEEP_READS);
    if (((uint32_t)result != DAP_TRANSFER_OK) || ((result >> 32) != 0U)) {
        return;
    }
    for (uint32_t i = 1U; i < SWEEP_READS; i++) {
        if (sweep_words[i] != sweep_words[0]) {
            return;
        }
    }
    // DPIDR's bit 0 reads as one.
    out->passed = (sweep_words[0] & 1U) != 0U;
    out->word = sweep_words[0];
}

void swd_sweep_watch(const uint8_t* request) {
    if (*request == ID_DAP_SWJ_Sequence) {
        const uint32_t count = request[1] != 0U ? request[1] : 256U;
        swd_connect_sequence(&swd_sweep_line, request + 2, count);
    } else if (*request == ID_DAP_Connect) {
        known_count = 0U;
    } else if (*request == ID_DAP_SWJ_Clock) {
        // A clock with less margin than the last one can need the latency the last one did not.
        const uint32_t hz = (uint32_t)request[1] | ((uint32_t)request[2] << 8) | ((uint32_t)request[3] << 16) |
                            ((uint32_t)request[4] << 24);
        if (hz != asked_hz) {
            asked_hz = hz;
            swd_sweep_reclocked = true;
        }
    }
}

/** Whether a connection should measure the latency in use: the generated code leaves it under
 * CONNECT_MARGIN cycles on a side, or a capture could start or stop under it. Either rebuilds a
 * masked build with or without a stall, which serves other latencies than this connection's read
 * checks; a measured one both serve. */
static bool unsure_latency(void) {
    if (!swd_gen_ready()) {
        return false;
    }
    if (dap_shared.energy_available && swd_gen_info.masked) {
        return true;
    }
    const int32_t latency = (int32_t)swd_gen_latency;
    const int32_t margins[] = {swd_gen_info.read_last - latency, latency - swd_gen_info.read_first,
                               swd_gen_info.write_last - latency, latency - swd_gen_info.write_first};
    for (uint32_t i = 0U; i < sizeof margins / sizeof margins[0]; i++) {
        if (margins[i] < CONNECT_MARGIN) {
            return true;
        }
    }
    return false;
}

/** The target the last line reset selected, among those measured, or nullptr. */
static struct known* known_target(void) {
    for (uint32_t i = 0U; i < known_count; i++) {
        if ((known[i].multidrop == swd_sweep_line.multidrop) && (known[i].targetsel == swd_sweep_line.targetsel)) {
            return &known[i];
        }
    }
    return nullptr;
}

/** Measures for the target the last line reset selected, and keeps what it found. A target that
 * did not answer, or whose reads contradicted each other, is measured again at its next
 * connection. */
static void measure_target(void) {
    swd_sweep();
    const uint32_t outcome = swd_sweep_last.result.outcome;
    if ((outcome != LATENCY_MEASURED) && (outcome != LATENCY_AMBIGUOUS) && (outcome != LATENCY_UNRESOLVED) &&
        (outcome != LATENCY_BOUNDARY)) {
        return;
    }
    struct known* target = known_target();
    if (target == nullptr) {
        target = &known[known_count < KNOWN_TARGETS ? known_count++ : KNOWN_TARGETS - 1U];
    }
    *target = (struct known){swd_sweep_line.multidrop, swd_sweep_line.targetsel, swd_gen_latency};
}

/** The host's line reset again, and its TARGETSEL on a multidrop wire, in probe-rs's form. A read
 * whose OK acknowledge the probe misread as WAIT or FAULT, with DAP_Data's data phase off, stops
 * clocking while the target still sends, and the target is out of step until a line reset. The
 * reset is longer than a host's: the target can take up to 36 of its clocks as the rest of that
 * read, the data, parity and turnaround, and a line reset needs 50 high after them. */
static void reselect(void) {
    static const uint8_t line_reset[] = {0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
                                         0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0x00U}; // 88 high, 3 low
    SWJ_Sequence(91U, line_reset);
    if (swd_sweep_line.multidrop) {
        const uint64_t bits = swd_connect_targetsel(swd_sweep_line.targetsel);
        uint8_t targetsel[SWD_CONNECT_TARGETSEL_CLOCKS / 8U];
        for (uint32_t i = 0U; i < sizeof targetsel; i++) {
            targetsel[i] = (uint8_t)(bits >> (8U * i));
        }
        SWJ_Sequence(SWD_CONNECT_TARGETSEL_CLOCKS, targetsel);
    }
}

static uint32_t run_transfer(const uint8_t* request, uint8_t* response) {
    *response = *request;
    return (1U << 16) + 1U + SWD_TransferCommand(request + 1, response + 1);
}

/** The latency for the target the last line reset selected, at the clock in use: what it measured
 *  for that target since the host's DAP_Connect, or a measurement where the generated reads leave
 *  too little margin. True if it measured. */
static bool settle_latency(void) {
    const struct known* target = known_target();
    if (target != nullptr) {
        if (target->latency != swd_gen_latency) {
            swd_gen_latency = target->latency;
            swd_gen_changed();
        }
        return false;
    }
    if (unsure_latency()) {
        measure_target();
        return true;
    }
    return false;
}

/** Out of line and in flash: inlined, it would take the measurement into SRAM with it. */
__attribute__((noinline)) uint32_t swd_sweep_connect(const uint8_t* request, uint8_t* response) {
    bool const reset = swd_sweep_line.reset;
    bool const reclocked = swd_sweep_reclocked;
    swd_sweep_line.reset = false;
    swd_sweep_reclocked = false;
    // [1] the DAP index, [2] the transfer count, [3] the first transfer's request. With nothing
    // generated the transfer refuses, which must not read as a misread DPIDR worth a line reset.
    if (!swd_sweep_automatic || (request[2] == 0U) || !swd_gen_ready()) {
        return run_transfer(request, response);
    }
    // A new clock with no line reset, or no DPIDR read after one: the measurement's own DPIDR reads
    // change nothing on the target, but without the host's DPIDR read there is no failure to rescue.
    if (!reset || ((request[3] & ~DAP_TRANSFER_TIMESTAMP) != DPIDR_READ)) {
        if (reclocked) {
            (void)settle_latency();
        }
        return run_transfer(request, response);
    }
    bool const measured = settle_latency();
    const uint32_t length = run_transfer(request, response);
    // [1] the transfers that completed: none, so the DPIDR read did not answer right.
    if (measured || (response[1] != 0U)) {
        return length;
    }
    // A known target that fails now has moved, as some do after a reset.
    const uint32_t before = swd_gen_latency;
    reselect();
    measure_target();
    if (swd_gen_latency == before) {
        return length;
    }
    const uint32_t again = run_transfer(request, response);
    if (response[1] != 0U) {
        swd_sweep_last.rescues++;
    }
    return again;
}

void swd_sweep(void) {
    const uint32_t start = DWT->CYCCNT;
    // Measured at the clock in use, whoever asked.
    swd_sweep_reclocked = false;
    struct sweep_context context = {.config = swd_gen_inputs()};
    context.config.data_phase = 1U;
    // The hold below keeps DMA0 off the port.
    context.config.stall = 0U;
    context.read_period =
        context.config.period < SWD_GEN_READ_PERIOD_MIN ? SWD_GEN_READ_PERIOD_MIN : context.config.period;
    static const struct latency_wire wire = {.safe_period = SWD_GEN_SAFE_PERIOD, .open_below = false};
    const bool held = energy_monitor_hold();
    latency_measure(&wire, context.read_period, swd_gen_latency, try_latency, &context, &swd_sweep_last.result);
    energy_monitor_release(held);
    if (swd_sweep_last.result.latency != 0U) {
        swd_gen_latency = swd_sweep_last.result.latency;
    }
    // SRAMX holds the last candidate's code, which nothing describes.
    gen_owner = GEN_OWNER_NONE;
    swd_gen_changed();
    (void)swd_gen_ready();
    swd_sweep_last.cycles = DWT->CYCCNT - start;
    swd_sweep_last.runs++;
}

/** Vendor 0x89 takes `latency, mode`. `latency` is the cycles from a rising-edge store to
 * the first load that sees the bit it launched, 1 to 64, or 0 to leave it as it is; setting it
 * regenerates and pins it, as mode 2 does. `mode` then:
 *
 *     0  nothing more
 *     1  measure the latency now, at the current clock, with DPIDR reads (src/swd_sweep.h)
 *     2  pin the latency: the probe stops measuring it on its own
 *     3  let the probe measure it on its own again
 *
 * It answers `status, latency, masked, ok`, then little-endian u32s: the clock asked for in Hz,
 * the write period and the read period in core cycles, the generated code's size in bytes, and
 * how many configurations were generated and how many could not be. Then the last measurement:
 *
 *     [29]      1 while the probe measures on its own, 0 once pinned
 *     [30]      what it found, LATENCY_* (latency.h)
 *     [31]      the latency it set, or 0 if it kept the one in use
 *     [32]      generations it ran reads with, at most 255
 *     [33]      read periods it measured at, the host's included
 *     [34..37]  the DPIDR value that read right
 *     [38..41]  core cycles it took
 *     [42..45]  measurements since the probe started
 *     [46..53]  u64: the latencies every read agrees with, bit n - 1 for latency n
 *     [54..57]  connections whose DPIDR read failed and read right once measured
 *
 * A measurement that ends LATENCY_INCONSISTENT runs again, up to LATENCY_ATTEMPTS in all; [32]
 * and [33] count every attempt's. One that ends LATENCY_BOUNDARY leaves [46..53] empty.
 *
 * Status is DAP_ERROR for a latency or a mode out of range, which change nothing, and for a
 * measurement asked for while the port is not plain SWD, which does not run. `ok` is 0 when the
 * current configuration could not be generated and transfers fail without touching the wire.
 */
uint32_t swd_sweep_command(const uint8_t* request, uint8_t* response) {
    const uint32_t latency = request[1];
    const uint32_t mode = request[2];
    response[1] = DAP_OK;
    if ((latency > LATENCY_MAX) || (mode > SWD_GEN_MODE_AUTOMATIC)) {
        response[1] = DAP_ERROR;
    } else {
        if (latency != 0U) {
            swd_gen_latency = latency;
            swd_sweep_automatic = false;
            swd_gen_changed();
        }
        if (mode == SWD_GEN_MODE_PIN) {
            swd_sweep_automatic = false;
        } else if (mode == SWD_GEN_MODE_AUTOMATIC) {
            swd_sweep_automatic = true;
        } else if (mode == SWD_GEN_MODE_MEASURE) {
            if ((DAP_Data.debug_port == DAP_PORT_SWD) && !cJTAG_Active()) {
                swd_sweep();
            } else {
                response[1] = DAP_ERROR;
            }
        }
    }
    (void)swd_gen_ready();
    response[2] = (uint8_t)swd_gen_latency;
    response[3] = swd_gen_info.masked ? 1U : 0U;
    response[4] = swd_gen_info.ok ? 1U : 0U;
    const struct latency_result* measured = &swd_sweep_last.result;
    uint8_t* out = put_u32_le(response + 5, swd_gen_clock_hz);
    out = put_u32_le(out, swd_gen_info.write_period);
    out = put_u32_le(out, swd_gen_info.read_period);
    out = put_u32_le(out, swd_gen_info.code_bytes);
    out = put_u32_le(out, swd_gen_builds);
    out = put_u32_le(out, swd_gen_failures);
    *out++ = swd_sweep_automatic ? 1U : 0U;
    *out++ = (uint8_t)measured->outcome;
    *out++ = (uint8_t)measured->latency;
    *out++ = (uint8_t)(measured->tries < 255U ? measured->tries : 255U);
    *out++ = (uint8_t)measured->periods;
    out = put_u32_le(out, measured->word);
    out = put_u32_le(out, swd_sweep_last.cycles);
    out = put_u32_le(out, swd_sweep_last.runs);
    out = put_u64_le(out, measured->consistent);
    out = put_u32_le(out, swd_sweep_last.rescues);
    return (uint32_t)(out - response);
}
