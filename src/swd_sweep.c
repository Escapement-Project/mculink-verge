/** The SWD read latency, measured on the wire. See swd_sweep.h and swd_latency.h. */
#include "swd_sweep.h"

#include "DAP_config.h"

#include "DAP.h"
#include "gen.h"
#include "swd_connect.h"
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

static void try_latency(uint32_t period, uint32_t latency, struct swd_latency_try* out, void* context) {
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

/** Whether the generated code for the latency in use leaves under CONNECT_MARGIN cycles on a side. */
static bool short_margin(void) {
    if (!swd_gen_ready()) {
        return false;
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

/** Out of line and in flash: inlined, it would take the measurement into SRAM with it. */
__attribute__((noinline)) void swd_sweep_connect(const uint8_t* request) {
    swd_sweep_line.reset = false;
    // [1] the DAP index, [2] the transfer count, [3] the first transfer's request.
    if (!swd_sweep_automatic || (request[2] == 0U) || ((request[3] & ~DAP_TRANSFER_TIMESTAMP) != DPIDR_READ)) {
        return;
    }
    for (uint32_t i = 0U; i < known_count; i++) {
        if ((known[i].multidrop == swd_sweep_line.multidrop) && (known[i].targetsel == swd_sweep_line.targetsel)) {
            if (known[i].latency != swd_gen_latency) {
                swd_gen_latency = known[i].latency;
                swd_gen_changed();
            }
            return;
        }
    }
    if (!short_margin()) {
        return;
    }
    swd_sweep();
    const uint32_t outcome = swd_sweep_last.result.outcome;
    if ((outcome == SWD_LATENCY_MEASURED) || (outcome == SWD_LATENCY_AMBIGUOUS) ||
        (outcome == SWD_LATENCY_UNRESOLVED)) {
        // A target that did not answer is measured again at its next connection.
        const uint32_t slot = known_count < KNOWN_TARGETS ? known_count++ : KNOWN_TARGETS - 1U;
        known[slot] = (struct known){swd_sweep_line.multidrop, swd_sweep_line.targetsel, swd_gen_latency};
    }
}

void swd_sweep(void) {
    const uint32_t start = DWT->CYCCNT;
    struct sweep_context context = {.config = swd_gen_inputs()};
    context.config.data_phase = 1U;
    context.read_period =
        context.config.period < SWD_GEN_READ_PERIOD_MIN ? SWD_GEN_READ_PERIOD_MIN : context.config.period;
    swd_latency_measure(context.read_period, swd_gen_latency, try_latency, &context, &swd_sweep_last.result);
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
