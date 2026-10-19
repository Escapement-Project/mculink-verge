/** The four-wire and two-pin read latency, measured on the wire. See jtag_sweep.h and latency.h. */
#include "jtag_sweep.h"

#include "DAP_config.h"

#include "DAP.h"
#include "cjtag.h"
#include "gen.h"
#include "jtag_dp.h"
#include "jtag_gen.h"

/** IDCODE reads at each candidate, as SWD reads DPIDR. */
#define SWEEP_READS 64U

struct jtag_sweep_result jtag_sweep_last;
bool jtag_sweep_automatic = true;
bool jtag_sweep_configured;

/** Cycles of margin below which a connection measures: how much later than the latency in use TDO
 * may come and still read right. */
#define CONNECT_MARGIN 2

extern uint32_t arm_DAP_ProcessCommand(const uint8_t* request, uint8_t* response);

static uint32_t current_wire(void) { return cJTAG_Active() ? JTAG_GEN_TWO_PIN : JTAG_GEN_FOUR_WIRE; }

static void try_latency(uint32_t period, uint32_t latency, struct latency_try* out, void* context) {
    struct jtag_gen_config config = *(const struct jtag_gen_config*)context;
    config.latency = latency;
    config.period = period;
    config.earliest = 1U;
    // jgen_ReadIDCode clocks through jtag_gen_scan.
    if (!jtag_gen_build(&config, &jtag_gen_scan, NULL, NULL)) {
        return;
    }
    out->generated = true;
    out->first = 1;
    out->last = jtag_gen_info.scan_last;
    out->same_below = jtag_gen_info.same_below;
    const uint32_t word = jgen_ReadIDCode();
    for (uint32_t i = 1U; i < SWEEP_READS; i++) {
        if (jgen_ReadIDCode() != word) {
            return;
        }
    }
    // IEEE 1149.1 fixes an IDCODE's bit 0 at one.
    out->passed = (word & 1U) != 0U;
    out->word = word;
}

void jtag_sweep_watch(const uint8_t* request) {
    if (*request == ID_DAP_JTAG_Configure) {
        jtag_sweep_configured = true;
    } else if (*request == ID_DAP_Connect) {
        jtag_sweep_configured = false;
    }
}

/** Whether the code generated for the latency in use samples less than CONNECT_MARGIN cycles after
 * it anywhere. */
static bool short_margin(uint32_t wire) {
    if (!jtag_gen_ready(wire)) {
        return false;
    }
    int32_t last = jtag_gen_info.scan_last;
    last = jtag_gen_info.read_last < last ? jtag_gen_info.read_last : last;
    last = jtag_gen_info.write_last < last ? jtag_gen_info.write_last : last;
    return last - (int32_t)jtag_gen_latency[wire] < CONNECT_MARGIN;
}

static void measure_target(uint32_t index) {
    DAP_Data.jtag_dev.index = index;
    jtag_sweep();
}

/** Out of line and in flash: inlined, it would take the measurement into SRAM with it. */
__attribute__((noinline)) uint32_t jtag_sweep_connect(const uint8_t* request, uint8_t* response) {
    jtag_sweep_configured = false;
    const uint32_t wire = current_wire();
    // [1] the DAP index, [2] the transfer count.
    if (!jtag_sweep_automatic || (request[2] == 0U) || (request[1] >= DAP_Data.jtag_dev.count)) {
        return arm_DAP_ProcessCommand(request, response);
    }
    // Nothing measured before is kept: probe-rs configures the chain again after it resets the
    // target, which can move the latency. The default reads a CC13xx in either state.
    jtag_gen_latency[wire] = wire == JTAG_GEN_TWO_PIN ? JTAG_GEN_LATENCY_DEFAULT : JTAG_GEN_FOUR_WIRE_LATENCY_DEFAULT;
    const bool measured = short_margin(wire);
    if (measured) {
        measure_target(request[1]);
    }
    const uint32_t length = arm_DAP_ProcessCommand(request, response);
    // [1] the transfers that completed: none, so the first did not answer right.
    if (measured || (response[1] != 0U)) {
        return length;
    }
    const uint32_t before = jtag_gen_latency[wire];
    measure_target(request[1]);
    if (jtag_gen_latency[wire] == before) {
        return length;
    }
    const uint32_t again = arm_DAP_ProcessCommand(request, response);
    if (response[1] != 0U) {
        jtag_sweep_last.rescues++;
    }
    return again;
}

void jtag_sweep(void) {
    const uint32_t start = DWT->CYCCNT;
    const uint32_t wire = current_wire();
    // This configuration has its measurement.
    jtag_sweep_configured = false;
    // Through the code generated for the latency in use: an IR scan reads nothing.
    JTAG_IR(JTAG_IDCODE);
    struct jtag_gen_config config = jtag_gen_inputs(wire);
    static const struct latency_wire search = {.safe_period = JTAG_GEN_SAFE_PERIOD, .open_below = true};
    latency_measure(&search, config.period, jtag_gen_latency[wire], try_latency, &config, &jtag_sweep_last.result);
    if (jtag_sweep_last.result.latency != 0U) {
        jtag_gen_latency[wire] = jtag_sweep_last.result.latency;
    }
    // SRAMX holds the last candidate's code, which nothing describes.
    gen_owner = GEN_OWNER_NONE;
    (void)jtag_gen_ready(wire);
    jtag_sweep_last.wire = wire;
    jtag_sweep_last.cycles = DWT->CYCCNT - start;
    jtag_sweep_last.runs++;
}
