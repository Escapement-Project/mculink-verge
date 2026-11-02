/** The SWD read latency, measured on the wire. See swd_sweep.h and latency.h. */
#include "swd_sweep.h"

#include "DAP_config.h"

#include "DAP.h"
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
static struct known known[KNOWN_TARGETS];
static uint32_t known_count;

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

/** Out of line and in flash: inlined, it would take the measurement into SRAM with it. */
__attribute__((noinline)) uint32_t swd_sweep_connect(const uint8_t* request, uint8_t* response) {
    swd_sweep_line.reset = false;
    // [1] the DAP index, [2] the transfer count, [3] the first transfer's request. With nothing
    // generated the transfer refuses, which must not read as a misread DPIDR worth a line reset.
    if (!swd_sweep_automatic || (request[2] == 0U) || ((request[3] & ~DAP_TRANSFER_TIMESTAMP) != DPIDR_READ) ||
        !swd_gen_ready()) {
        return run_transfer(request, response);
    }
    bool measured = false;
    const struct known* target = known_target();
    if (target != nullptr) {
        if (target->latency != swd_gen_latency) {
            swd_gen_latency = target->latency;
            swd_gen_changed();
        }
    } else if (unsure_latency()) {
        measure_target();
        measured = true;
    }
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
