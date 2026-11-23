/** The JTAG and cJTAG read latency, measured on the wire. See jtag_sweep.h and latency.h. */
#include "jtag_sweep.h"

#include "DAP_config.h"

#include "DAP.h"
#include "byte_io.h"
#include "cjtag.h"
#include "energy_monitor.h"
#include "gen.h"
#include "jtag_dp.h"
#include "jtag_gen.h"

/** IDCODE reads at each candidate. They are most of a JTAG or cJTAG measurement, so a quarter of
 *  the 64 DPIDR reads SWD makes (src/swd_sweep.c). A latency on a cycle boundary reads right only
 *  some of the time, and the search keeps a build only when every read agrees. */
#define SWEEP_READS 16U

/** Cycles past the measured latency that cJTAG builds sample. The measurement finds the first
 *  cycle on which every IDCODE read agrees, which leaves transfers no margin: on the Pro's CC1312
 *  about one 256 KiB OScan5 or OScan6 read in three failed on an acknowledge above 20 MHz at the
 *  measured latency, and none one cycle later. */
#define TWO_PIN_SLACK 1U

struct jtag_sweep_result jtag_sweep_last;
bool jtag_sweep_automatic = true;
bool jtag_sweep_configured;

/** Cycles of margin below which a connection measures: how much later than the latency in use TDO
 * may come and still read right. */
#define CONNECT_MARGIN 2

extern uint32_t arm_DAP_ProcessCommand(const uint8_t* request, uint8_t* response);


static void try_latency(uint32_t period, uint32_t latency, struct latency_try* out, void* context) {
    struct jtag_gen_config config = *(const struct jtag_gen_config*)context;
    config.latency = latency;
    config.period = period;
    config.earliest = 1U;
    // jgen_ReadIDCode clocks through jtag_gen_scan.
    if (!jtag_gen_build(&config, &jtag_gen_scan, nullptr, nullptr)) {
        return;
    }
    out->generated = true;
    out->first = jtag_gen_info.scan_first;
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

/** The clock the host last asked for, to tell a change from the same one again. */
static uint32_t asked_hz;

void jtag_sweep_watch(const uint8_t* request) {
    if (*request == ID_DAP_JTAG_Configure) {
        jtag_sweep_configured = true;
    } else if (*request == ID_DAP_Connect) {
        jtag_sweep_configured = false;
    } else if (*request == ID_DAP_SWJ_Clock) {
        // A clock with less margin than the last one can need the latency the last one did not, so
        // the next transfer settles it as the first after DAP_JTAG_Configure does.
        const uint32_t hz = (uint32_t)request[1] | ((uint32_t)request[2] << 8) | ((uint32_t)request[3] << 16) |
                            ((uint32_t)request[4] << 24);
        if (hz != asked_hz) {
            asked_hz = hz;
            jtag_sweep_configured = true;
        }
    }
}

/** Whether the code generated for the latency in use samples less than CONNECT_MARGIN cycles after
 * it anywhere. At the default latency every build a stall moves does, so a capture starting or
 * stopping never changes code a connection's read checked without measuring
 * (tools/jtag_latency_test.c). No build's range starts above 6, so at the default the side below
 * always has margin. */
static bool short_margin(uint32_t wire) {
    if (!jtag_gen_ready_transfers(wire)) {
        return false;
    }
    int32_t last = jtag_gen_info.scan_last;
    last = jtag_gen_info.read_last < last ? jtag_gen_info.read_last : last;
    last = jtag_gen_info.write_last < last ? jtag_gen_info.write_last : last;
    return last - (int32_t)jtag_gen_latency[JTAG_GEN_PINS(wire)] < CONNECT_MARGIN;
}

static void measure_target(uint32_t index) {
    DAP_Data.jtag_dev.index = index;
    jtag_sweep();
}

/** Out of line and in flash: inlined, it would take the measurement into SRAM with it. */
__attribute__((noinline)) uint32_t jtag_sweep_connect(const uint8_t* request, uint8_t* response) {
    jtag_sweep_configured = false;
    const uint32_t wire = jtag_gen_wire();
    // [1] the DAP index, [2] the transfer count.
    if (!jtag_sweep_automatic || (request[2] == 0U) || (request[1] >= DAP_Data.jtag_dev.count)) {
        return arm_DAP_ProcessCommand(request, response);
    }
    // Nothing measured before is kept: probe-rs configures the chain again after it resets the
    // target, which can move the latency. The default reads a CC13xx in either state.
    jtag_gen_latency[JTAG_GEN_PINS(wire)] =
        wire == JTAG_GEN_FOUR_WIRE ? JTAG_GEN_FOUR_WIRE_LATENCY_DEFAULT : JTAG_GEN_LATENCY_DEFAULT;
    const bool measured = short_margin(wire);
    if (measured) {
        measure_target(request[1]);
    }
    const uint32_t length = arm_DAP_ProcessCommand(request, response);
    // [1] the transfers that completed: none, so the first did not answer right.
    if (measured || (response[1] != 0U)) {
        return length;
    }
    const uint32_t before = jtag_gen_latency[JTAG_GEN_PINS(wire)];
    measure_target(request[1]);
    if (jtag_gen_latency[JTAG_GEN_PINS(wire)] == before) {
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
    const uint32_t wire = jtag_gen_wire();
    // This configuration has its measurement.
    jtag_sweep_configured = false;
    // Through the code generated for the latency in use: an IR scan reads nothing.
    JTAG_IR(JTAG_IDCODE);
    struct jtag_gen_config config = jtag_gen_inputs(wire);
    // The hold below keeps DMA0 off the port.
    config.stall = 0U;
    // Every measuring build samples before the fall that ends its bit (jtag_gen.h, `earliest`).
    static const struct latency_wire search = {.safe_period = JTAG_GEN_SAFE_PERIOD, .open_below = true};
    const uint32_t pins = JTAG_GEN_PINS(wire);
    const bool held = energy_monitor_hold();
    latency_measure(&search, config.period, jtag_gen_latency[pins], try_latency, &config, &jtag_sweep_last.result);
    energy_monitor_release(held);
    const uint32_t measured = jtag_sweep_last.result.latency;
    if (measured != 0U) {
        const uint32_t latency = pins == JTAG_GEN_TWO_PIN ? measured + TWO_PIN_SLACK : measured;
        jtag_gen_latency[pins] = latency < LATENCY_MAX ? latency : LATENCY_MAX;
    }
    // SRAMX holds the last candidate's code, which nothing describes.
    gen_owner = GEN_OWNER_NONE;
    (void)jtag_gen_ready_transfers(wire);
    jtag_sweep_last.wire = wire;
    jtag_sweep_last.cycles = DWT->CYCCNT - start;
    jtag_sweep_last.runs++;
}

/** Vendor 0x8A takes `two_pin, four_wire, mode`: each wire's cycles from the falling edge
 * that launches TDO to the first load that sees it, 1 to 64, or 0 to leave it as it is. Setting
 * either regenerates the wire in use and pins both, as mode 2 does. `mode` is 0x89's,
 * for the wire in use: 1 measures it now with IDCODE reads of the TAP at the last transfer's chain
 * index (src/jtag_sweep.h), which leaves that TAP's IR at IDCODE.
 *
 * It answers `status, two_pin, four_wire, masked, ok`, then little-endian u32s: the clock asked for
 * in Hz, the period (a JTAG clock, or a cJTAG packet) and a JTAG clock in core cycles, the
 * generated code's size in bytes, and how many configurations were generated and how many could
 * not be. Then the last measurement:
 *
 *     [30]      1 while the probe measures on its own, 0 once pinned
 *     [31]      what it found, LATENCY_* (latency.h)
 *     [32]      the wire it measured, JTAG_GEN_* (jtag_gen.h)
 *     [33]      the latency it measured, or 0 if it kept the one in use; cJTAG uses one more, at
 *               most 64 (src/jtag_sweep.c)
 *     [34]      generations it ran reads with, at most 255, every attempt's (as 0x89)
 *     [35]      periods it measured at, the host's included, every attempt's
 *     [36..39]  the IDCODE that read right
 *     [40..43]  core cycles it took
 *     [44..47]  measurements since the probe started
 *     [48..55]  u64: the latencies every read agrees with, bit n - 1 for latency n; empty after
 *               LATENCY_BOUNDARY
 *     [56..59]  connections whose first transfer failed and ran right once measured
 *     [60..63]  configurations copied back from a kept build with transfers, not generated
 *
 * Status is DAP_ERROR for a latency or a mode out of range, which change nothing, and for a
 * measurement asked for while the port is not JTAG, which does not run. `ok` is 0 when the current
 * configuration could not be generated and commands fail without touching the wire; its bit 1 says whole
 * transfers were generated too, rather than run as scans, and bit 2 that their data bits are a
 * loop.
 */
uint32_t jtag_sweep_command(const uint8_t* request, uint8_t* response) {
    const uint32_t two_pin = request[1];
    const uint32_t four_wire = request[2];
    const uint32_t mode = request[3];
    response[1] = DAP_OK;
    if ((two_pin > LATENCY_MAX) || (four_wire > LATENCY_MAX) || (mode > SWD_GEN_MODE_AUTOMATIC)) {
        response[1] = DAP_ERROR;
    } else {
        if (two_pin != 0U) {
            jtag_gen_latency[JTAG_GEN_TWO_PIN] = two_pin;
            jtag_sweep_automatic = false;
        }
        if (four_wire != 0U) {
            jtag_gen_latency[JTAG_GEN_FOUR_WIRE] = four_wire;
            jtag_sweep_automatic = false;
        }
        if (mode == SWD_GEN_MODE_PIN) {
            jtag_sweep_automatic = false;
        } else if (mode == SWD_GEN_MODE_AUTOMATIC) {
            jtag_sweep_automatic = true;
        } else if (mode == SWD_GEN_MODE_MEASURE) {
            // The measurement scans at the index the host's last transfer left, which ARM's
            // handlers store before they refuse one past the chain.
            if ((DAP_Data.debug_port == DAP_PORT_JTAG) && (DAP_Data.jtag_dev.index < DAP_Data.jtag_dev.count)) {
                jtag_sweep();
            } else {
                response[1] = DAP_ERROR;
            }
        }
    }
    (void)jtag_gen_ready(jtag_gen_wire());
    response[2] = (uint8_t)jtag_gen_latency[JTAG_GEN_TWO_PIN];
    response[3] = (uint8_t)jtag_gen_latency[JTAG_GEN_FOUR_WIRE];
    response[4] = jtag_gen_info.masked ? 1U : 0U;
    response[5] = (jtag_gen_info.ok ? 1U : 0U) | (jtag_gen_info.transfers ? 2U : 0U) | (jtag_gen_info.looped ? 4U : 0U);
    uint8_t* out = put_u32_le(response + 6, jtag_gen_clock_hz);
    out = put_u32_le(out, jtag_gen_info.period);
    out = put_u32_le(out, jtag_gen_info.frame);
    out = put_u32_le(out, jtag_gen_info.code_bytes);
    out = put_u32_le(out, jtag_gen_builds);
    out = put_u32_le(out, jtag_gen_failures);
    const struct latency_result* measured = &jtag_sweep_last.result;
    *out++ = jtag_sweep_automatic ? 1U : 0U;
    *out++ = (uint8_t)measured->outcome;
    *out++ = (uint8_t)jtag_sweep_last.wire;
    *out++ = (uint8_t)measured->latency;
    *out++ = (uint8_t)(measured->tries < 255U ? measured->tries : 255U);
    *out++ = (uint8_t)measured->periods;
    out = put_u32_le(out, measured->word);
    out = put_u32_le(out, jtag_sweep_last.cycles);
    out = put_u32_le(out, jtag_sweep_last.runs);
    out = put_u64_le(out, measured->consistent);
    out = put_u32_le(out, jtag_sweep_last.rescues);
    out = put_u32_le(out, jtag_gen_restores);
    return (uint32_t)(out - response);
}

/** Vendor 0x8B takes `mode`: 0 turns the learned idle clocks off, so the generated wire
 * is ARM's exactly, 1 turns them on (the default), anything else leaves them. It answers `status,
 * on, paced`: whether pacing is on and the idle clocks learned so far (jtag_gen_paced).
 */
uint32_t jtag_pacing_command(const uint8_t* request, uint8_t* response) {
    if (request[1] <= 1U) {
        jtag_gen_pacing = request[1] != 0U;
        if (DAP_Data.debug_port == DAP_PORT_JTAG) {
            (void)jtag_gen_ready(jtag_gen_wire());
        }
    }
    response[1] = DAP_OK;
    response[2] = jtag_gen_pacing ? 1U : 0U;
    response[3] = (uint8_t)jtag_gen_paced;
    return 4U;
}

/** Vendor 0x8D takes `n` and answers `status`, then generation n back from the newest, 0
 * the newest: the count of generations so far as a little-endian u32, then struct
 * jtag_gen_log_entry's fields in order, the last three little-endian (jtag_gen.h). Status is
 * DAP_ERROR, and nothing follows, for an n the log no longer holds.
 */
uint32_t jtag_log_command(const uint8_t* request, uint8_t* response) {
    const uint32_t count = jtag_gen_builds + jtag_gen_failures + jtag_gen_restores;
    const uint32_t back = request[1];
    if ((back >= count) || (back >= JTAG_GEN_LOG_SIZE)) {
        response[1] = DAP_ERROR;
        return 2U;
    }
    const struct jtag_gen_log_entry* entry = &jtag_gen_log[(count - 1U - back) % JTAG_GEN_LOG_SIZE];
    response[1] = DAP_OK;
    uint8_t* out = put_u32_le(response + 2, count);
    const uint8_t bytes[] = {entry->command, entry->wire,  entry->changed, entry->flags, entry->latency,
                             entry->before,  entry->after, entry->idle,    entry->paced};
    memcpy(out, bytes, sizeof bytes);
    out += sizeof bytes;
    out = put_u16_le(out, entry->period);
    out = put_u32_le(out, entry->at);
    out = put_u32_le(out, entry->cycles);
    return (uint32_t)(out - response);
}
