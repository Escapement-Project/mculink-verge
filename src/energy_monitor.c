#include "energy_monitor.h"
#include "energy_samples.h"

#include <stddef.h>

#include "fsl_clock.h"
#include "fsl_device_registers.h"
#include "fsl_iocon.h"
#include "fsl_lpadc.h"
#include "fsl_power.h"
#include "fsl_reset.h"

#include "dap_shared.h"
#include "dap_core.h"
#include "dio.h"
#include "DAP_config.h"
#include "board_pins.h"
#include "core_clock.h"
#include "dma_ring.h"
#include "energy_integration.h"
#include "target_sense.h"
#if defined(SWD_CODEGEN) || defined(JTAG_CODEGEN)
#include "gen.h"
#endif
#ifdef SWD_CODEGEN
#include "swd_gen.h"
#endif

#define ENERGY_DMA_SOURCE (&ADC0->RESFIFO[0])

/** The ring's first half raises INTA as it completes and the second INTB, so a completion CPU 0 has
 *  not taken when the next one comes is not lost in one flag (energy_segment_flag). */
#define ENERGY_SEGMENT_XFERCFG(words, half)                                                          \
    (DMA_CHANNEL_XFERCFG_CFGVALID_MASK | DMA_CHANNEL_XFERCFG_RELOAD_MASK |                           \
     (((half) == 0U) ? DMA_CHANNEL_XFERCFG_SETINTA_MASK : DMA_CHANNEL_XFERCFG_SETINTB_MASK) |         \
     DMA_CHANNEL_XFERCFG_WIDTH(2U) | DMA_CHANNEL_XFERCFG_SRCINC(0U) | DMA_CHANNEL_XFERCFG_DSTINC(1U) | \
     DMA_CHANNEL_XFERCFG_XFERCOUNT((words) - 1U))
static_assert(ENERGY_SEGMENTS == 2U, "a completion flag for each half");

/** Ring words a pair takes on average: its three results, and the empty reads after them. The
 *  watermark request is still raised for up to two reads after the one that empties the FIFO, and
 *  those read an empty FIFO. The voltage's and the current's results land together: a pair took
 *  6.4 words up to 3,000 pairs a second and 6.86 to 6.98 from 10,000 to 100,000, measured with the
 *  ADC at 18.75 MHz. Segment sizes follow the high rates' figure; below 4,572 pairs a second every
 *  segment is the 16-word floor. Converted one after the other, a pair took 9. UM11126 asks for a watermark of at least 2 with DMA, and the watermark stays at 0
 *  regardless: at 2 the request sometimes stops one result short, so a pair's last results wait in
 *  the FIFO for the next pair's and the latest reply runs a pair behind. */
#define ENERGY_WORDS_PER_PAIR 7U
/** CPU 0's interrupt has to integrate a segment before the next one completes, so two segment ends
 *  must not come closer together than that takes. Any 16 words span a gap between pairs. */
#define ENERGY_SEGMENT_MIN_WORDS 16U

/** Cycles a DMA0 read of ADC0 can hold a load from the secure GPIO, which shares its AHB port
 *  (BUSES.md). Measured; no longer hold was seen. */
#define ADC0_LOAD_STALL 2U

/** ADC clocks from a trigger to its pair's first conversion: an allowance, not measured. */
#define ENERGY_TRIGGER_LATENCY_ADCK 4U
/** The least time between a pair's end and the next trigger that CPU 0 aims a re-arm into: 2 us. */
#define ENERGY_REARM_MIN_GAP_TICKS (ENERGY_TIMER_HZ / 500000U)

static uint32_t requested_rate_hz;
static uint32_t actual_rate_hz;
/** What the next capture's start uses (`0x90` mode 14). That start takes it and leaves automatic,
 *  so a host that never asks always gets the default. */
static uint8_t next_ranges = ENERGY_RANGES_AUTO;
/** Set by energy_monitor_start_stream for the start it makes: Verge Aux's stream begins with it. */
static bool stream_on_start;
static uint8_t stream_dio_mask;
static bool stream_identified;
static bool identity_initialized;
static bool identity_failed;
static uint8_t identity_boot[16];
static uint64_t capture_counter;
static energy_capture_id_t next_identity;
static bool stamp_ready;
/** The conversion timer's count from one toggle of its output to the next: a pair every two. */
static uint32_t trigger_ticks;
/** A read-latency hold: when its first trigger was due by CTIMER4, and how far that was from the
 *  stop in the conversion timer's own count. */
static uint32_t hold_due;
static uint32_t hold_to_next;

#define ENERGY_CONTROL_POLLS 1000000U
static bool pins_ready;
static bool controls_ready;
static bool inputmux_ready;
static target_sense_current_range_t current_range;
static energy_calibration_t low_range_calibration;
static energy_calibration_t high_range_calibration;
/** The high range's no-load reading, the amplifier's output floor; 0 until a calibration reads it. */
static uint16_t high_floor_code;
static bool current_calibrated;
static bool accumulator_started;

/** The pair latest last answered with in this capture. A read can find the ring a segment short
 *  (ring_written), which would answer an older pair than one already given: a host reads a sequence
 *  going back as a new start. */
typedef struct {
    uint32_t sequence;
    uint16_t voltage;
    uint16_t current;
    uint16_t range;
} latest_answer_t;
static latest_answer_t latest_answer;

static_assert(ADC_RESFIFO_VALID_MASK == 0x80000000U && ADC_RESFIFO_TSRC_SHIFT == 16U,
              "energy_samples.h must match the ADC result layout");

/** CPU 0's, which CPU 1 reaches only through this pointer. */
static struct energy_integration* integration(void) {
    return dap_shared.energy_integration;
}

static bool capture_failed(void) {
    return energy_capture_failed(integration());
}

static void accumulator_reset(void) {
    energy_accumulator_write_begin();
    dap_shared.energy_accumulator_pj = 0U;
    dap_shared.energy_accumulator_pairs = 0U;
    dap_shared.energy_accumulator_fraction_q32 = 0U;
    energy_accumulator_write_end();
    integration()->decoder = (energy_sample_decoder_t){0};
    integration()->consumed_bytes = 0U;
    latest_answer = (latest_answer_t){0};
}

static void controls_init(void) {
    if ((board_kind != BOARD_KIND_PRO) || controls_ready) {
        return;
    }

    static const uint8_t pins[] = {24U, 22U, 23U, 26U, 29U};
    const uint32_t mask = (1UL << 24U) | (1UL << 22U) | (1UL << 23U) | (1UL << 26U) | (1UL << 29U);
    CLOCK_EnableClock(kCLOCK_Iocon);
    CLOCK_EnableClock(kCLOCK_Gpio1);
    GPIO->CLR[1] = mask;
    for (uint32_t i = 0U; i < sizeof(pins) / sizeof(pins[0]); i++) {
        IOCON_PinMuxSet(IOCON, 1U, pins[i], IOCON_FUNC0 | IOCON_MODE_INACT | IOCON_DIGITAL_EN);
    }
    GPIO->DIRSET[1] = mask;
    controls_ready = true;
}

static void pins_init(void) {
    controls_init();
    if ((board_kind != BOARD_KIND_PRO) || pins_ready) {
        return;
    }
    CLOCK_EnableClock(kCLOCK_Iocon);
    CLOCK_EnableClock(kCLOCK_Gpio0);
    CLOCK_EnableClock(kCLOCK_Gpio1);
    CLOCK_EnableClock(kCLOCK_Comp);

    GPIO->CLR[0] = 1UL << 17U;
    GPIO->DIRSET[0] = 1UL << 17U;
    GPIO->DIRCLR[0] = 1UL << 30U;

    IOCON_PinMuxSet(IOCON, 0U, 15U,
                    IOCON_FUNC0 | IOCON_MODE_INACT | IOCON_ANALOG_EN | IOCON_PIO_ASW_MASK);
    IOCON_PinMuxSet(IOCON, 0U, 23U, IOCON_FUNC0 | IOCON_MODE_INACT | IOCON_ANALOG_EN | IOCON_PIO_ASW_MASK);
    IOCON_PinMuxSet(IOCON, 0U, 16U, IOCON_FUNC0 | IOCON_MODE_INACT | IOCON_ANALOG_EN | IOCON_PIO_ASW_MASK);
    IOCON_PinMuxSet(IOCON, 1U, 8U, IOCON_FUNC0 | IOCON_MODE_INACT | IOCON_ANALOG_EN | IOCON_PIO_ASW_MASK);
    IOCON_PinMuxSet(IOCON, 1U, 14U,
                    IOCON_FUNC0 | IOCON_MODE_INACT | IOCON_ANALOG_EN | IOCON_PIO_ASW_MASK);
    // ACMPVREF, tied to the 3.0 V reference. Its analog switch resets open.
    IOCON_PinMuxSet(IOCON, 1U, 19U,
                    IOCON_FUNC0 | IOCON_MODE_INACT | IOCON_ANALOG_EN | IOCON_PIO_ASW_MASK);
    IOCON_PinMuxSet(IOCON, 0U, 17U, IOCON_FUNC0 | IOCON_MODE_INACT | IOCON_DIGITAL_EN);
    IOCON_PinMuxSet(IOCON, 0U, 29U, IOCON_FUNC7 | IOCON_SLEW_FAST | IOCON_DIGITAL_EN);
    IOCON_PinMuxSet(IOCON, 0U, 30U, IOCON_FUNC0 | IOCON_MODE_INACT | IOCON_DIGITAL_EN);

    // BOARDS.md, "Range switching": the amplified current on the negative input, the ladder at
    // 28/31 of ACMPVREF on the positive. The output falls, and clears U18, above 90 mV on the
    // shunt.
    POWER_DisablePD(kPDRUNCFG_PD_COMP);
    PMC->COMP = PMC_COMP_HYST(0U) | PMC_COMP_VREFINPUT(0U) | PMC_COMP_LOWPOWER(0U) | PMC_COMP_PMUX(0U) |
                PMC_COMP_NMUX(4U) | PMC_COMP_VREF(28U);
    pins_ready = true;
}

void energy_monitor_init(void) {
    controls_init();
    pins_init();
    dap_shared.energy_accumulator_sequence = 0U;
    dap_shared.energy_dma_sequence = 0U;
    dap_shared.energy_accumulator_pj = 0U;
    dap_shared.energy_accumulator_pairs = 0U;
    dap_shared.energy_accumulator_fraction_q32 = 0U;
    dap_shared.energy_range_retry_pending = 0U;
    dap_shared.energy_range_retry_enabled = 0U;
    dap_shared.energy_control_request = 0U;
    dap_shared.energy_control_ack = 0U;
}

/** How long each calibration step waits before it converts. With 1 ms, the first calibration
 *  after flashing read the no-load point at the amplifier's floor, and 5 ms read it right; from
 *  1 ms up, the slopes of later calibrations agree with 100 ms. */
#define CALIBRATION_SETTLE_US 20000U

/** Out of line, so tools/energy_checks.py can skip it. */
__attribute__((noinline)) static void calibration_settle(void) {
    uint32_t const start = DWT->CYCCNT;
    while ((DWT->CYCCNT - start) < CALIBRATION_SETTLE_US * (CORE_CLOCK_HZ / 1000000U)) {
    }
}

/** A calibration load and its GPIO port 1 switch, from node T to ground (schematic sheet 7). The
 *  switch is a PMF250XNE driven at 3.3 V, which its data sheet puts between 212 mohm (4.5 V) and
 *  269 mohm (2.5 V), typical. */
typedef struct {
    uint32_t pin;
    uint32_t milliohms;
} calibration_load_t;

#define LOAD_SWITCH_MILLIOHMS 250U
/** No load: only the divider's current. */
static const calibration_load_t no_load = {0U, 0U};
static const calibration_load_t load_r1 = {1UL << 22U, 6650000U + LOAD_SWITCH_MILLIOHMS};
static const calibration_load_t load_r2 = {1UL << 23U, 66500U + LOAD_SWITCH_MILLIOHMS};
static const calibration_load_t load_r3 = {1UL << 26U, 1650000U + LOAD_SWITCH_MILLIOHMS};
static const calibration_load_t load_r4 = {1UL << 29U, 16500U + LOAD_SWITCH_MILLIOHMS};
#define CAL_DISC (1UL << 24U)

/** Each range's loads, smallest current first (BOARDS.md, "Calibration"). The divider's current
 *  alone is a point only in the low ranges: in the high ones it sits under the amplifier's output
 *  floor. Without R1, the 350 mA setting's high range read 0.5 mA 4% high, extending R3's slope
 *  down. R1 counts only clear of the floor. */
static const calibration_load_t* const loads_350_low[] = {&no_load, &load_r1, &load_r3, nullptr};
static const calibration_load_t* const loads_350_high[] = {&load_r1, &load_r3, &load_r2, &load_r4, nullptr};
static const calibration_load_t* const loads_50_low[] = {&no_load, &load_r1, nullptr};
static const calibration_load_t* const loads_50_high[] = {&load_r1, &load_r3, &load_r2, nullptr};

/** 99.8% of full scale. The loads put up to 97 mV on a range that ends at 100 mV, so a unit whose
 *  tolerances add up can saturate; below this the 128 averaged conversions stay clear of 65535. */
#define CALIBRATION_CODE_LIMIT 65400U
/** Two points closer than this give no slope worth having. */
#define CALIBRATION_MIN_STEP 64U
/** How far over a high range's no-load reading, the amplifier's output floor, a point has to be.
 *  A point pinned at the floor reads the currents between it and the next point low. In the 350 mA
 *  setting R1 lands 18 to 27 codes over the floor at the nominal resistance, by the amplifier's
 *  offset, and higher with the resistance J17's contact adds (BOARDS.md, "The floor"). */
#define CALIBRATION_FLOOR_MARGIN 24U
/** Within this many codes of the floor a high-range reading is unreliable: the stream marks the
 *  pair under range (docs/host-interface.md). */
#define UNDER_RANGE_MARGIN 64U
/** Node T with the probe's 3.3 V on the shunts. Less means CTRL_CAL_DISC did not switch it. */
#define CALIBRATION_MIN_MV 2500U

typedef enum : uint8_t {
    POINT_OK,
    POINT_SATURATED,
    POINT_FAILED,
} calibration_point_t;

static bool calibration_read_pair(uint16_t* voltage_code, uint16_t* current_code) {
    uint32_t voltage_sum = 0U;
    uint32_t current_sum = 0U;
    for (uint32_t i = 0U; i < 4U; i++) {
        uint16_t voltage = 0U;
        uint16_t current = 0U;
        if (target_sense_convert_pair_for_calibration(&voltage, &current) != TARGET_SENSE_OK) {
            return false;
        }
        voltage_sum += voltage;
        current_sum += current;
    }
    *voltage_code = (uint16_t)((voltage_sum + 2U) / 4U);
    *current_code = (uint16_t)((current_sum + 2U) / 4U);
    return true;
}

/** The current through the shunt with `load` on, besides the divider's. */
static calibration_point_t calibration_point(const calibration_load_t* load, uint16_t* code,
                                             uint32_t* current_na) {
    GPIO->SET[1] = load->pin;
    calibration_settle();
    uint16_t voltage_code = 0U;
    bool const read_ok = calibration_read_pair(&voltage_code, code);
    GPIO->CLR[1] = load->pin;
    uint32_t const full_scale_mv = target_sense_full_scale_mv();
    if (!read_ok || energy_voltage_mv(voltage_code, full_scale_mv) < CALIBRATION_MIN_MV) {
        return POINT_FAILED;
    }
    if (*code > CALIBRATION_CODE_LIMIT) {
        return POINT_SATURATED;
    }
    // Node T in millivolts, times 65536.
    uint64_t const node = (uint64_t)voltage_code * 2U * full_scale_mv;
    uint64_t current = node * 1000000U / ((uint64_t)ENERGY_DIVIDER_OHMS << 16U);
    if (load->milliohms != 0U) {
        current += node * 1000000000U / ((uint64_t)load->milliohms << 16U);
    }
    *current_na = (uint32_t)current;
    return POINT_OK;
}

/** Fills `calibration` with the points kept so far, whether or not the range succeeds. A point
 *  under `min_code` is left out. */
static bool calibration_range(const calibration_load_t* const* loads, uint32_t min_code,
                              energy_calibration_t* calibration) {
    *calibration = (energy_calibration_t){0};
    for (; (*loads != nullptr) && (calibration->count < 4U); loads++) {
        uint16_t code = 0U;
        uint32_t current_na = 0U;
        calibration_point_t const result = calibration_point(*loads, &code, &current_na);
        if (result == POINT_FAILED) {
            return false;
        }
        if (result == POINT_SATURATED) {
            break;
        }
        if (code < min_code) {
            continue;
        }
        uint8_t const count = calibration->count;
        if (count > 0U) {
            if ((code <= calibration->code[count - 1U]) || (current_na <= calibration->current_na[count - 1U])) {
                return false;
            }
            if (((uint32_t)code - calibration->code[count - 1U]) < CALIBRATION_MIN_STEP) {
                continue;
            }
        }
        calibration->code[count] = code;
        calibration->current_na[count] = current_na;
        calibration->count = count + 1U;
    }
    for (uint32_t i = 0U; (i + 1U) < calibration->count; i++) {
        uint64_t const current_delta = calibration->current_na[i + 1U] - calibration->current_na[i];
        uint32_t const code_delta = calibration->code[i + 1U] - calibration->code[i];
        uint64_t const slope = (current_delta << 16U) / code_delta;
        if (slope > UINT32_MAX) {
            return false;
        }
        calibration->slope_na_per_code_q16[i] = (uint32_t)slope;
    }
    return calibration->count >= 2U;
}

/** U18's reset, CTRL_HIGH_CURR_DIS (PIO0_29): held low by the pin to keep the high range, or the
 *  comparator's, which clears the latch by itself past the threshold (BOARDS.md, "Range
 *  switching"). Calibration drives the pin itself and hands it back to the comparator. */
static void hold_high_range(bool hold) {
    uint32_t const p29 = 1UL << 29U;
    if (hold) {
        // Low before the pin takes over, so the latch sees no release on the way.
        GPIO->CLR[0] = p29;
        GPIO->DIRSET[0] = p29;
        IOCON->PIO[0][29] = IOCON_FUNC0 | IOCON_MODE_INACT | IOCON_DIGITAL_EN;
    } else {
        IOCON->PIO[0][29] = IOCON_FUNC7 | IOCON_SLEW_FAST | IOCON_DIGITAL_EN;
        GPIO->DIRCLR[0] = p29;
    }
}

/** U18's Q, CTRL_HIGH_CURR, read back on P0_30: high while Q3 is off and only the low range's
 *  shunt carries the current. */
static bool latch_in_low_range(void) {
    return (GPIO->PIN[0] & (1UL << 30U)) != 0U;
}

/** BOARDS.md, "Calibration". */
__attribute__((noinline)) static bool current_calibrate(void) {
    current_calibrated = false;
    low_range_calibration = (energy_calibration_t){0};
    high_range_calibration = (energy_calibration_t){0};
    high_floor_code = 0U;
    if (current_range == TARGET_SENSE_CURRENT_RANGE_UNKNOWN) {
        return false;
    }
    bool const wide = current_range == TARGET_SENSE_CURRENT_RANGE_350_MA;
    uint32_t const p29 = 1UL << 29U;
    uint32_t const p17 = 1UL << 17U;
    uint32_t const loads = load_r1.pin | load_r2.pin | load_r3.pin | load_r4.pin;

    GPIO->CLR[1] = loads;
    GPIO->SET[1] = CAL_DISC;
    // U18's reset, held high here rather than by the comparator: the low range's loads go past
    // the threshold, and the comparator would clear the latch under them.
    GPIO->SET[0] = p29;
    GPIO->DIRSET[0] = p29;
    IOCON->PIO[0][29] = IOCON_FUNC0 | IOCON_MODE_INACT | IOCON_DIGITAL_EN;
    // A rising CTRL_HIGH_CURR_EN loads U18's tied-high D.
    GPIO->CLR[0] = p17;
    calibration_settle();
    GPIO->SET[0] = p17;
    calibration_settle();
    bool const low_ok = latch_in_low_range() &&
                        calibration_range(wide ? loads_350_low : loads_50_low, 0U, &low_range_calibration);

    GPIO->CLR[0] = p29;
    calibration_settle();
    uint16_t floor_code = 0U;
    uint32_t floor_na = 0U;
    bool const high_ok = !latch_in_low_range() && (calibration_point(&no_load, &floor_code, &floor_na) == POINT_OK) &&
                         calibration_range(wide ? loads_350_high : loads_50_high,
                                           floor_code + CALIBRATION_FLOOR_MARGIN, &high_range_calibration);
    high_floor_code = floor_code;

    GPIO->CLR[1] = CAL_DISC | loads;
    IOCON->PIO[0][29] = IOCON_FUNC7 | IOCON_SLEW_FAST | IOCON_DIGITAL_EN;
    GPIO->DIRCLR[0] = p29;
    GPIO->CLR[0] = p17;
    calibration_settle();

    current_calibrated = low_ok && high_ok;
    return current_calibrated;
}

static energy_calibration_t const* range_calibration(bool low) {
    return low ? &low_range_calibration : &high_range_calibration;
}

/** UM11673 Table 4's low-range limits. The comparator trips above both (BOARDS.md, "Range
 *  switching"), so a high-range segment that stays under its setting's limit re-arms the low range. */
static uint32_t rearm_below_na(void) {
    return current_range == TARGET_SENSE_CURRENT_RANGE_350_MA ? 1500000U : 400000U;
}

/** About a millisecond of pairs, so the low range re-arms within about 2 ms of the current falling
 *  at every rate from 2,286 pairs/s (BOARDS.md, "Range switching"): the most words that take no
 *  longer, as a power of two. With 10 ms, the re-arm took 7 to 15 ms at 20,000 pairs/s and 1.5 to
 *  3 ms at 100,000, so the range a short low phase was read on depended on the rate.
 *
 *  CPU 0's DMA interrupt has to integrate each segment before the next one completes, so within
 *  about 0.5 ms at the shortest: the next completion is DMA0 starting on that half again. A capture
 *  whose interrupt comes later fails (energy_integrate). */
static uint32_t segment_words(uint32_t rate_hz) {
    uint32_t const words_in_1_ms = (rate_hz * ENERGY_WORDS_PER_PAIR) / 1000U;
    uint32_t words = ENERGY_SEGMENT_MAX_WORDS;
    while ((words > ENERGY_SEGMENT_MIN_WORDS) && (words > words_in_1_ms)) {
        words /= 2U;
    }
    return words;
}

/** A low phase shorter than this, after a re-arm from the band, is the low range giving way
 *  (struct energy_integration): 3 ms, and at least two pairs, so that a phase of one pair counts at
 *  any rate. BOARDS.md ("Range switching") has the low phases it was set between. */
static uint32_t short_low_pairs(uint32_t rate_hz) {
    uint32_t const pairs = ((rate_hz * 3U) + 999U) / 1000U;
    return pairs > 2U ? pairs : 2U;
}

/** Where the backoff judges a short low phase (struct energy_integration): the pair half a
 *  millisecond after the switch up, and at least the one after it. The recharge at a switch up
 *  lifts the pairs after it for about 140 us at 100,000 pairs/s. */
static uint32_t verdict_pairs(uint32_t rate_hz) {
    uint32_t const pairs = (rate_hz + 1999U) / 2000U;
    return pairs > 1U ? pairs : 1U;
}

/** The backoff's longest hold: about a second of segments. */
static uint32_t hold_max_segments(uint32_t rate_hz, uint32_t words) {
    uint32_t const segments = (rate_hz * ENERGY_WORDS_PER_PAIR) / words;
    return segments > 1U ? segments : 1U;
}

static uint32_t ring_written(void) {
    uint32_t const mask = ENERGY_DMA_MASK;
    uint32_t const segment = integration()->segment_words;
    for (;;) {
        uint32_t const sequence = dap_shared.energy_dma_sequence;
        __DMB();
        if ((sequence & 1U) != 0U) {
            continue;
        }
        uint32_t const counted = dap_shared.energy_segments - integration()->segments_at_start;
        __DMB();
        volatile uint32_t const* const flag = energy_segment_flag(counted);
        bool const pending = (*flag & mask) != 0U;
        uint32_t const xfercfg = DMA0->CHANNEL[ENERGY_DMA_CHANNEL].XFERCFG;
        bool const still_pending = (*flag & mask) != 0U;
        bool const overtaken = (*energy_segment_flag(counted + 1U) & mask) != 0U;
        __DMB();
        if ((pending == still_pending) && (counted == (dap_shared.energy_segments - integration()->segments_at_start)) &&
            (sequence == dap_shared.energy_dma_sequence)) {
            // A segment CPU 0 has not taken, and the next one completed: the ring this position
            // counts as unread may be under rewrite. Failed here, so a caller's next
            // capture_failed() sees it even if CPU 0 clears both flags before storing its own.
            if (overtaken) {
                dap_shared.energy_dma_error = 1U;
            }
            uint32_t const left = ((xfercfg >> DMA_XFERCOUNT_SHIFT) & DMA_XFERCOUNT_MASK) + 1U;
            // A descriptor that has run out reads 1024 to go (src/dma_ring.h), which is the next
            // segment's start at every size a capture uses.
            uint32_t const words = ((counted + (pending ? 1U : 0U)) * segment) + ((segment - left) & (segment - 1U));
            return words * sizeof(uint32_t);
        }
    }
}

static bool capture_drain(void) {
    struct energy_integration* const energy = integration();
    if (!energy_consume_until(energy, ring_written())) {
        return false;
    }
    energy_accumulator_write_begin();
    energy_total_t total = energy_accumulator_values();
    bool empty = false;
    for (uint32_t i = 0U; i < 32U; i++) {
        uint32_t const result = ADC0->RESFIFO[0];
        if ((result & ADC_RESFIFO_VALID_MASK) == 0U) {
            empty = true;
            break;
        }
        energy_consume_result(energy, result, &total);
    }
    energy_flush_held(energy, &total);
    // An incomplete tuple belongs to this capture, even when the next one uses the same rate.
    energy->decoder.phase = ENERGY_EXPECT_VOLTAGE;
    energy_accumulator_publish(&total, energy->scale_q32);
    energy_accumulator_write_end();
    if (!empty) {
        dap_shared.energy_dma_error = 1U;
    }
    return empty;
}

/** CTIMER4 runs free from PLL0, the conversion timer's clock, so a stream's times count the clock
 *  its pairs are paced by (src/energy_stream.h). */
static void stamp_init(void) {
    if (stamp_ready) {
        return;
    }
    CLOCK_AttachClk(kPLL0_to_CTIMER4);
    CLOCK_EnableClock(kCLOCK_Timer4);
    RESET_PeripheralReset(kCTIMER4_RST_SHIFT_RSTn);
    CTIMER4->PR = 0U;
    CTIMER4->MCR = 0U;
    CTIMER4->TCR = CTIMER_TCR_CEN_MASK;
    stamp_ready = true;
}

/** CTIMER2 toggles the trigger every half period of this many ticks; it rises once a period. */
static uint32_t timer_ticks(uint32_t rate_hz) {
    return (ENERGY_TIMER_HZ + rate_hz) / (2U * rate_hz);
}

static uint32_t timer_effective_rate(uint32_t rate_hz) {
    return ENERGY_TIMER_HZ / (2U * timer_ticks(rate_hz));
}

/** The rate the timer runs at for `rate_hz`, to the nearest pair a second: what a reply reports. A
 *  stream's header gives it exactly, as a clock and a period. */
static uint32_t timer_rounded_rate(uint32_t rate_hz) {
    uint32_t const ticks = timer_ticks(rate_hz);
    return (ENERGY_TIMER_HZ + ticks) / (2U * ticks);
}

/** The stand-in's weight and CPU 0's re-arm window, for the timer `requested_hz` starts and the
 *  averaging `rate_hz` gets (src/energy_pairs.h, src/main.c). The
 *  re-arm comes in the first half of the time between a pair's end and the next trigger, which
 *  leaves the switch at least as long to settle as it had to land; a rate with under
 *  ENERGY_REARM_MIN_GAP_TICKS of it re-arms anywhere. */
static void switch_timing(struct energy_integration* energy, uint32_t requested_hz, uint32_t rate_hz) {
    uint32_t const ticks = timer_ticks(requested_hz);
    uint32_t const window = target_sense_monitor_window_adck(rate_hz) * TARGET_SENSE_ADC_DIVIDER;
    uint64_t const weight = (((uint64_t)window << 16U) + 2U * ticks) / (4U * ticks);
    energy->switch_weight_q16 = weight < 32768U ? (uint32_t)weight : 32768U;
    uint32_t const busy = (target_sense_monitor_pair_adck(rate_hz) + ENERGY_TRIGGER_LATENCY_ADCK) * TARGET_SENSE_ADC_DIVIDER;
    uint32_t const gap = (2U * ticks > busy) ? (2U * ticks - busy) : 0U;
    energy->trigger_ticks = ticks;
    energy->rearm_between_pairs = gap >= ENERGY_REARM_MIN_GAP_TICKS;
    energy->rearm_from_ticks = busy;
    energy->rearm_until_ticks = busy + gap / 2U;
}

/** Returns CTIMER4 as the timer starts; the first conversion comes `*ticks` later. */
static uint32_t timer_start(uint32_t rate_hz, uint32_t* ticks_out) {
    uint32_t const ticks = timer_ticks(rate_hz);
    actual_rate_hz = ENERGY_TIMER_HZ / (2U * ticks);
    trigger_ticks = ticks;
    *ticks_out = ticks;

    CLOCK_AttachClk(kPLL0_to_CTIMER2);
    CLOCK_EnableClock(kCLOCK_Timer2);
    RESET_PeripheralReset(kCTIMER2_RST_SHIFT_RSTn);
    CTIMER2->TCR = CTIMER_TCR_CRST_MASK;
    CTIMER2->PR = 0U;
    CTIMER2->MR[3] = ticks - 1U;
    CTIMER2->MCR = CTIMER_MCR_MR3R_MASK;
    CTIMER2->EMR = CTIMER_EMR_EMC3(3U);
    CTIMER2->IR = 0x0FU;
    uint32_t const started = CTIMER4->TC;
    CTIMER2->TCR = CTIMER_TCR_CEN_MASK;
    return started;
}

/** Out of line, so tools/image.py can call it. */
__attribute__((noinline)) static void capture_ring_init(uint32_t words) {
    struct energy_integration* const energy = integration();
    uint32_t const source = (uint32_t)ENERGY_DMA_SOURCE;
    energy->segment_words = words;
    for (uint32_t i = 0U; i < ENERGY_SEGMENTS; i++) {
        energy->descriptors[i].xfercfg = ENERGY_SEGMENT_XFERCFG(words, i);
        energy->descriptors[i].source_end = source;
        energy->descriptors[i].destination_end = (uint32_t)&energy->ring[((i + 1U) * words) - 1U];
        energy->descriptors[i].link = (uint32_t)&energy->descriptors[(i + 1U) % ENERGY_SEGMENTS];
    }

    volatile dma_descriptor_t* const entry = &dap_shared.dma_table[ENERGY_DMA_CHANNEL];
    entry->xfercfg = 0U;
    entry->source_end = source;
    entry->destination_end = (uint32_t)&energy->ring[words - 1U];
    entry->link = energy->descriptors[0].link;
}

/** CPU 0 acknowledges from its loop only. */
static uint32_t control_request(bool quiet) {
    uint32_t const requested = ((dap_shared.energy_control_request & ~DAP_ENERGY_QUIET_FLAG) + 2U) |
                               (quiet ? DAP_ENERGY_QUIET_FLAG : 0U);
    dap_shared.energy_control_request = requested;
    __DMB();
    MAILBOX->MBOXIRQ[DAP_CORE_MAILBOX_CPU0].IRQSET = DAP_CORE_MAILBOX_WAKE_MASK;
    return requested;
}

static bool capture_quiet(bool quiet) {
    // A pause inside a start, calibrating at boot, or reading the supply after a stop: this core
    // holds the state already. It asks only when CPU 0 has it, so a request withdrawn below goes
    // back to where the state was.
    uint32_t const current = dap_shared.energy_control_request;
    if (quiet && ((current & DAP_ENERGY_QUIET_FLAG) != 0U) && (dap_shared.energy_control_ack == current)) {
        return true;
    }
    uint32_t const requested = control_request(quiet);
    for (uint32_t i = 0U; i < ENERGY_CONTROL_POLLS; i++) {
        if (dap_shared.energy_control_ack == requested) {
            __DMB();
            return true;
        }
    }
    dap_shared.energy_dma_error = 1U;
    if (quiet) {
        // Withdrawn: a loop that comes back late would echo it and leave the state, and any stream
        // open in it, to this core, which has given up. CPU 0 keeps the state it had, and its loop
        // ends the stream (src/aux.c).
        (void)control_request(false);
    }
    return false;
}

static void capture_stream_begin(struct energy_integration* energy, bool identified) {
    struct energy_stream* const stream = &energy->stream;
    if (identified) {
        stream->sent = stream->written;
    }
    uint8_t levels = 0U;
    uint8_t const pins = dio_stream_pins(stream_dio_mask, &levels);
    energy_stream_header_t const header = {
        .clock_hz = ENERGY_TIMER_HZ,
        .period = 2U * timer_ticks(requested_rate_hz),
        .setting = energy->setting,
        .full_scale_mv = (uint16_t)energy->full_scale_mv,
        .first_sequence = 1U,
        .previous_low = energy->previous_low,
        .low = &low_range_calibration,
        .high = &high_range_calibration,
        .dio_mask = pins,
        .dio_levels = levels,
        .dio_stamp = CTIMER4->TC,
        .edge_resolution_ns = (pins != 0U) ? energy_stream_edge_resolution(ENERGY_TIMER_HZ) : 0U,
        .capture_id = identified ? &next_identity : nullptr,
        .switch_weight_q16 = energy->switch_weight_q16,
        .rearm_between_pairs = energy->rearm_between_pairs,
        .high_range_only = energy->high_range_only,
    };
    (void)energy_stream_begin(stream, &header);
}

__attribute__((noinline)) static bool energy_capture_start(bool new_session) {
    struct energy_integration* const energy = integration();
    actual_rate_hz = timer_effective_rate(requested_rate_hz);
    capture_ring_init(segment_words(actual_rate_hz));
    DMA0->COMMON[0].ENABLECLR = ENERGY_DMA_MASK;
    DMA0->COMMON[0].ABORT = ENERGY_DMA_MASK;
    DMA0->COMMON[0].INTENCLR = ENERGY_DMA_MASK;
    DMA0->COMMON[0].ERRINT = ENERGY_DMA_MASK;
    DMA0->COMMON[0].INTA = ENERGY_DMA_MASK;
    DMA0->COMMON[0].INTB = ENERGY_DMA_MASK;
    energy->segments_at_start = dap_shared.energy_segments;
    if (new_session) {
        energy->high_range_only = next_ranges == ENERGY_RANGES_HIGH;
        next_ranges = ENERGY_RANGES_AUTO;
    }
    hold_high_range(energy->high_range_only);
    dap_shared.energy_range_retry_pending = 0U;
    dap_shared.energy_range_retry_enabled = energy->high_range_only ? 0U : 1U;
    energy->on_high_range = false;
    energy->rearm_blocked = false;
    energy->previous_low = !energy->high_range_only;
    energy->held = false;
    energy->previous_known = false;
    switch_timing(energy, requested_rate_hz, actual_rate_hz);
    energy->decoder.phase = ENERGY_EXPECT_VOLTAGE;
    energy->consumed_bytes = 0U;
    LPADC_DoResetFIFO0(ADC0);
    // A pair's share of a second is its period over the clock, which no whole rate gives exactly:
    // asked 7, the timer runs at 6.99999987 pairs a second.
    energy->scale_q32 = ((uint64_t)(2U * timer_ticks(requested_rate_hz)) << 32U) / ENERGY_TIMER_HZ;
    energy->low_range = low_range_calibration;
    energy->high_range = high_range_calibration;
    energy->calibrated = current_calibrated;
    energy->full_scale_mv = target_sense_full_scale_mv();
    energy->rearm_below_na = rearm_below_na();
    energy->band_from_na = energy->rearm_below_na / 2U;
    energy->short_low_pairs = short_low_pairs(actual_rate_hz);
    energy->verdict_pairs = verdict_pairs(actual_rate_hz);
    energy->hold_max_segments = hold_max_segments(actual_rate_hz, energy->segment_words);
    energy->hold_segments = 1U;
    energy->clean_segments = 0U;
    energy->low_pairs = 0U;
    energy->verdict_in = 0U;
    energy->rearmed_in_band = false;
    energy->in_band = false;
    energy->under_range_code = current_calibrated ? (uint16_t)(high_floor_code + UNDER_RANGE_MARGIN) : 0U;
    energy->setting = (current_range == TARGET_SENSE_CURRENT_RANGE_350_MA) ? 1U
                      : (current_range == TARGET_SENSE_CURRENT_RANGE_50_MA) ? 2U : 0U;
    stamp_init();
    struct energy_stream* const stream = &energy->stream;
    if (new_session) {
        // CPU 0's loop ends a stream it finds with a failure recorded, so the last capture's has to
        // go before the handback. A handback that fails records its own.
        dap_shared.energy_dma_error = 0U;
        __DMB();
        energy_stream_end(stream, ENERGY_STREAM_END_RESTARTED, energy->decoder.sequence);
        energy_stream_timeline_prepare(stream, 1U);
        if (stream_on_start) {
            if (!stream_identified) {
                capture_stream_begin(energy, false);
            }
        }
    } else {
        energy_stream_timeline_prepare(stream, energy->decoder.sequence + 1U);
    }
    target_sense_configure_monitor(actual_rate_hz);
    CLOCK_EnableClock(kCLOCK_InputMux);
    if (!inputmux_ready) {
        RESET_ReleasePeripheralReset(kMUX0_RST_SHIFT_RSTn);
        inputmux_ready = true;
    }
    INPUTMUX->DMA0_REQ_ENA_SET = ENERGY_DMA_MASK;
    LPADC_EnableFIFO0WatermarkDMA(ADC0, true);

    if (!capture_quiet(false)) {
        // The release stays asked for, so CPU 0 may take the state at any moment: take it back
        // first. If that fails too, CPU 0 keeps it, and its loop ends the stream (src/aux.c).
        if (capture_quiet(true)) {
            stream->timeline_pending = false;
            // The capture never ran, so its edges have no times.
            stream->edge_mask = 0U;
            energy_stream_end(stream, ENERGY_STREAM_END_FAULT, energy->decoder.sequence);
        }
        return false;
    }
    if (new_session) {
        energy->capture_id_sequence++;
        __DMB();
        energy->capture_id = next_identity;
        __DMB();
        energy->capture_id_sequence++;
        accumulator_reset();
        accumulator_started = true;
        ADC0->STAT = ADC_STAT_FOF0_MASK;
        if (stream_on_start && stream_identified) {
            capture_stream_begin(energy, true);
        }
    }
    energy->running = true;
    __DMB();
    DMA0->CHANNEL[ENERGY_DMA_CHANNEL].CFG = DMA_CHANNEL_CFG_PERIPHREQEN_MASK;
    DMA0->COMMON[0].INTENSET = ENERGY_DMA_MASK;
    DMA0->COMMON[0].ENABLESET = ENERGY_DMA_MASK;
    DMA0->CHANNEL[ENERGY_DMA_CHANNEL].XFERCFG =
        ENERGY_SEGMENT_XFERCFG(energy->segment_words, 0U) | DMA_CHANNEL_XFERCFG_SWTRIG_MASK;
    // An edge whatever P0_17 was left at: CPU 0's re-arm can raise it after a pause lowered it.
    if (!energy->high_range_only) {
        energy_latch_clock();
    }
    uint32_t ticks = 0U;
    uint32_t const started = timer_start(requested_rate_hz, &ticks);
    energy_stream_timeline_run(stream, new_session, ENERGY_TIMER_HZ, ticks, started);
    return true;
}

__attribute__((noinline)) bool energy_monitor_pause(void) {
    if (!dap_shared.energy_available) {
        return true;
    }
    dap_shared.energy_range_retry_enabled = 0U;
    dap_shared.energy_range_retry_pending = 0U;
    __DMB();
    if (integration()->running) {
        CTIMER2->TCR = 0U;
        __DSB();
        uint32_t adc_polls = 0U;
        while ((ADC0->STAT & ADC_STAT_ADC_ACTIVE_MASK) != 0U) {
            if (++adc_polls == ENERGY_CONTROL_POLLS) {
                dap_shared.energy_dma_error = 1U;
                return false;
            }
        }
        GPIO->CLR[0] = 1UL << 17U;
        LPADC_EnableFIFO0WatermarkDMA(ADC0, false);
        DMA0->COMMON[0].ENABLECLR = ENERGY_DMA_MASK;
        uint32_t polls = 0U;
        while ((DMA0->COMMON[0].BUSY & ENERGY_DMA_MASK) != 0U) {
            if (++polls == ENERGY_CONTROL_POLLS) {
                dap_shared.energy_dma_error = 1U;
                return false;
            }
        }
        DMA0->COMMON[0].INTENCLR = ENERGY_DMA_MASK;
    }
    if (!capture_quiet(true)) {
        return false;
    }
    if (!integration()->running) {
        return true;
    }
    struct energy_stream* const stream = &integration()->stream;
    if (capture_failed()) {
        energy_stream_end(stream, ENERGY_STREAM_END_FAULT, integration()->decoder.sequence);
    } else if (!capture_drain()) {
        energy_stream_end(stream, ENERGY_STREAM_END_FAULT, integration()->decoder.sequence);
        return false;
    } else {
        energy_stream_pause(stream);
    }
    integration()->decoder.phase = ENERGY_EXPECT_VOLTAGE;
    DMA0->COMMON[0].ABORT = ENERGY_DMA_MASK;
    DMA0->COMMON[0].INTA = ENERGY_DMA_MASK;
    DMA0->COMMON[0].INTB = ENERGY_DMA_MASK;
    DMA0->COMMON[0].ERRINT = ENERGY_DMA_MASK;
    INPUTMUX->DMA0_REQ_ENA_CLR = ENERGY_DMA_MASK;
    LPADC_DoResetFIFO0(ADC0);
    integration()->running = false;
    return true;
}

/** Out of line, so tools/energy_checks.py can call it. */
__attribute__((noinline)) bool energy_monitor_hold(void) {
    // A pause that failed part way leaves capture running with its timer stopped; release must not
    // start it.
    if (!dap_shared.energy_available || !integration()->running || ((CTIMER2->TCR & CTIMER_TCR_CEN_MASK) == 0U)) {
        return false;
    }
    CTIMER2->TCR = 0U;
    uint32_t const stopped = CTIMER4->TC;
    // Its output rises for each pair. Stopped with it high, a fall comes first; either way the count
    // runs to the match before the next toggle.
    uint32_t const count = CTIMER2->TC;
    uint32_t const to_toggle = (count < trigger_ticks) ? (trigger_ticks - 1U - count) : 0U;
    hold_to_next = to_toggle + (((CTIMER2->EMR & CTIMER_EMR_EM3_MASK) != 0U) ? trigger_ticks : 0U);
    hold_due = stopped + hold_to_next;
    __DSB();
    // The conversion under way, and DMA0's reads of what it leaves at the FIFO's watermark.
    for (uint32_t polls = 0U; polls < ENERGY_CONTROL_POLLS; polls++) {
        if (((ADC0->STAT & ADC_STAT_ADC_ACTIVE_MASK) == 0U) && ((DMA0->COMMON[0].BUSY & ENERGY_DMA_MASK) == 0U)) {
            break;
        }
    }
    return true;
}

__attribute__((noinline)) void energy_monitor_release(bool held) {
    if (held) {
        energy_stream_hold_t* const hold = energy_stream_hold(&integration()->stream, hold_due);
        uint32_t const resumed = CTIMER4->TC;
        if (hold != NULL) {
            hold->came = resumed + hold_to_next;
            __DMB();
        }
        CTIMER2->TCR = CTIMER_TCR_CEN_MASK;
    }
}

bool energy_monitor_resume(void) {
    if ((requested_rate_hz == 0U) || integration()->running) {
        return true;
    }
    if (dap_shared.energy_dma_error != 0U) {
        return false;
    }
    return energy_capture_start(false);
}

/** Calibration opens J9, so it runs again only when the last one failed or CURR_RANGE_SET reads
 *  another setting than the one calibrated. A setting that cannot be read leaves the tables alone. */
static bool calibrate_for_setting(void) {
    target_sense_current_range_t const setting = target_sense_current_range();
    if (setting == TARGET_SENSE_CURRENT_RANGE_UNKNOWN) {
        return false;
    }
    if (current_calibrated && (setting == current_range)) {
        return true;
    }
    current_range = setting;
    return current_calibrate();
}

/** Out of line, so tools/energy_checks.py can call it. */
__attribute__((noinline)) void energy_monitor_calibrate(void) {
    if (!dap_shared.energy_available) {
        return;
    }
    // CPU 1 starts before CPU 0 brings USB up and enters its loop, so take the state with no bound
    // on the wait. Each conversion's pause then finds it held and asks nothing (capture_quiet).
    uint32_t const requested = control_request(true);
    while (dap_shared.energy_control_ack != requested) {
    }
    __DMB();
    pins_init();
    if (target_sense_adc_init() == TARGET_SENSE_OK) {
        (void)calibrate_for_setting();
    }
}

/** While a capture is asked for, paused or not, the generated bit paths keep a stall's margin. */
static void request_rate(uint32_t rate_hz) {
    requested_rate_hz = rate_hz;
#if defined(SWD_CODEGEN) || defined(JTAG_CODEGEN)
    uint32_t const stall = (rate_hz != 0U) ? ADC0_LOAD_STALL : 0U;
    if (gen_stall != stall) {
        gen_stall = stall;
#ifdef SWD_CODEGEN
        // The JTAG generator compares its inputs on every call; SWD's waits to be told.
        swd_gen_changed();
#endif
    }
#endif
}

energy_monitor_status_t energy_monitor_start(uint32_t rate_hz) {
    if (!dap_shared.energy_available) {
        return ENERGY_MONITOR_UNAVAILABLE;
    }
    if ((rate_hz == 0U) || (rate_hz > ENERGY_MAX_RATE_HZ)) {
        return ENERGY_MONITOR_BAD_RATE;
    }
    // A failed initialization is retried by the next start, so one bad boot of the RNG does not
    // take capture away until the probe restarts.
    if (!identity_initialized) {
        identity_initialized = energy_identity_boot(identity_boot);
        identity_failed = !identity_initialized;
    }
    if (identity_failed || (capture_counter == UINT64_MAX)) {
        return ENERGY_MONITOR_IDENTITY_ERROR;
    }
    for (uint32_t i = 0U; i < 16U; i++) {
        next_identity.bytes[i] = identity_boot[i];
    }
    for (uint32_t i = 0U; i < 8U; i++) {
        next_identity.bytes[16U + i] = (uint8_t)((capture_counter + 1U) >> (8U * i));
    }
    if (!energy_monitor_pause()) {
        return ENERGY_MONITOR_DMA_ERROR;
    }
    request_rate(0U);
    actual_rate_hz = 0U;
    pins_init();
    if ((target_sense_adc_init() != TARGET_SENSE_OK) || !calibrate_for_setting()) {
        energy_stream_end(&integration()->stream, ENERGY_STREAM_END_STOPPED, integration()->decoder.sequence);
        return ENERGY_MONITOR_ADC_ERROR;
    }
    request_rate(rate_hz);
    if (!energy_capture_start(true)) {
        request_rate(0U);
        actual_rate_hz = 0U;
        return ENERGY_MONITOR_DMA_ERROR;
    }
    capture_counter++;
    return ENERGY_MONITOR_OK;
}

uint32_t energy_monitor_rate(void) { return (actual_rate_hz != 0U) ? timer_rounded_rate(requested_rate_hz) : 0U; }

energy_monitor_status_t energy_monitor_start_stream(uint32_t rate_hz, uint8_t dio_mask) {
    stream_on_start = true;
    stream_dio_mask = dio_mask;
    energy_monitor_status_t const status = energy_monitor_start(rate_hz);
    stream_on_start = false;
    return status;
}

energy_monitor_status_t energy_monitor_start_identified(uint32_t rate_hz, bool stream, uint8_t dio_mask) {
    stream_identified = true;
    energy_monitor_status_t const status = stream ? energy_monitor_start_stream(rate_hz, dio_mask)
                                                  : energy_monitor_start(rate_hz);
    stream_identified = false;
    return status;
}

energy_monitor_status_t energy_monitor_set_ranges(uint8_t ranges) {
    if (!dap_shared.energy_available) {
        return ENERGY_MONITOR_UNAVAILABLE;
    }
    if ((ranges != ENERGY_RANGES_AUTO) && (ranges != ENERGY_RANGES_HIGH)) {
        return ENERGY_MONITOR_BAD_ARGUMENT;
    }
    next_ranges = ranges;
    return ENERGY_MONITOR_OK;
}

energy_monitor_status_t energy_monitor_identity(energy_capture_id_t* id, uint32_t* rate) {
    *id = (energy_capture_id_t){0};
    *rate = 0U;
    if (!dap_shared.energy_available) {
        return ENERGY_MONITOR_UNAVAILABLE;
    }
    *id = integration()->capture_id;
    if (capture_failed()) {
        return ENERGY_MONITOR_DMA_ERROR;
    }
    if (identity_failed) {
        return ENERGY_MONITOR_IDENTITY_ERROR;
    }
    if (requested_rate_hz == 0U) {
        return ENERGY_MONITOR_STOPPED;
    }
    *rate = energy_monitor_rate();
    return ENERGY_MONITOR_OK;
}

energy_monitor_status_t energy_monitor_stop_identified(uint8_t const* id) {
    if (!dap_shared.energy_available) {
        return ENERGY_MONITOR_UNAVAILABLE;
    }
    if (!energy_identity_matches(&integration()->capture_id, id)) {
        return ENERGY_MONITOR_STALE_CAPTURE;
    }
    return energy_monitor_stop();
}

energy_monitor_status_t energy_monitor_stop(void) {
    if (!energy_monitor_pause()) {
        return ENERGY_MONITOR_DMA_ERROR;
    }
    energy_stream_end(&integration()->stream, ENERGY_STREAM_END_STOPPED, integration()->decoder.sequence);
    request_rate(0U);
    actual_rate_hz = 0U;
    CLOCK_DisableClock(kCLOCK_Timer2);
    CLOCK_AttachClk(kNONE_to_CTIMER2);
    return dap_shared.energy_dma_error != 0U ? ENERGY_MONITOR_DMA_ERROR : ENERGY_MONITOR_OK;
}

energy_monitor_status_t energy_monitor_latest(energy_monitor_sample_t* sample) {
    if (sample == nullptr) {
        return ENERGY_MONITOR_BAD_ARGUMENT;
    }
    if (!dap_shared.energy_available) {
        return ENERGY_MONITOR_UNAVAILABLE;
    }
    if (capture_failed()) {
        return ENERGY_MONITOR_DMA_ERROR;
    }
    struct energy_integration* const energy = integration();
    if (!energy->running) {
        return ENERGY_MONITOR_STOPPED;
    }
    uint32_t const ring_words = energy_ring_words(energy);
    energy_sample_decoder_t decoder;
    for (;;) {
        uint32_t const before = dap_shared.energy_accumulator_sequence;
        __DMB();
        if ((before & 1U) != 0U) {
            continue;
        }
        decoder = energy->decoder;
        uint32_t const consumed = energy->consumed_bytes;
        uint32_t const written = ring_written();
        __DMB();
        if (before != dap_shared.energy_accumulator_sequence) {
            continue;
        }
        uint32_t const bytes = written - consumed;
        if (bytes > (ring_words * sizeof(uint32_t))) {
            dap_shared.energy_dma_error = 1U;
            return ENERGY_MONITOR_DMA_ERROR;
        }
        for (uint32_t position = consumed; position != written; position += sizeof(uint32_t)) {
            uint32_t const index = (position / sizeof(uint32_t)) & (ring_words - 1U);
            (void)energy_sample_feed(&decoder, energy->ring[index]);
        }
        uint32_t const after = ring_written();
        __DMB();
        if (before != dap_shared.energy_accumulator_sequence) {
            continue;
        }
        if (after - consumed > (ring_words * sizeof(uint32_t))) {
            dap_shared.energy_dma_error = 1U;
            return ENERGY_MONITOR_DMA_ERROR;
        }
        break;
    }
    if (capture_failed()) {
        return ENERGY_MONITOR_DMA_ERROR;
    }
    if (!decoder.have_sample) {
        return ENERGY_MONITOR_NO_SAMPLE;
    }
    // Sequences wrap after 2^32 pairs, and a short read is at most a segment behind.
    if ((int32_t)(decoder.sequence - latest_answer.sequence) >= 0) {
        latest_answer = (latest_answer_t){decoder.sequence, decoder.last_voltage, decoder.last_current, decoder.last_range};
    }
    sample->voltage_code = latest_answer.voltage;
    sample->current_code = latest_answer.current;
    sample->range_code = latest_answer.range;
    sample->sequence = latest_answer.sequence;
    sample->voltage_mv = energy_voltage_mv(sample->voltage_code, target_sense_full_scale_mv());
    sample->range_id = 0U;
    sample->current_na = 0;
    sample->calibrated = false;
    if (current_range != TARGET_SENSE_CURRENT_RANGE_UNKNOWN) {
        bool const low = energy_low_range(sample->range_code);
        sample->range_id = (current_range == TARGET_SENSE_CURRENT_RANGE_350_MA) ? (low ? 1U : 2U)
                                                                                 : (low ? 3U : 4U);
        if (current_calibrated) {
            uint32_t const target_na = energy_target_na(
                energy_shunt_na(sample->current_code, range_calibration(low)), sample->voltage_mv);
            sample->current_na = (target_na > (uint32_t)INT32_MAX) ? INT32_MAX : (int32_t)target_na;
            sample->calibrated = true;
        }
    }
    return ENERGY_MONITOR_OK;
}

energy_monitor_status_t energy_monitor_calibration(bool high_range, energy_calibration_t* calibration) {
    if (calibration == nullptr) {
        return ENERGY_MONITOR_BAD_ARGUMENT;
    }
    if (!dap_shared.energy_available) {
        return ENERGY_MONITOR_UNAVAILABLE;
    }
    *calibration = *range_calibration(!high_range);
    return current_calibrated ? ENERGY_MONITOR_OK : ENERGY_MONITOR_ADC_ERROR;
}

uint8_t energy_monitor_mark(uint32_t number, uint8_t const* label, uint8_t length, bool* available) {
    struct energy_integration* const energy = integration();
    *available = (dap_shared.energy_available != 0U) && (energy != nullptr);
    if (!*available) {
        return ENERGY_STREAM_MARK_NO_CAPTURE;
    }
    if (!energy->running || capture_failed()) {
        return ENERGY_STREAM_MARK_NO_CAPTURE;
    }
    // CTimer 4 runs once a capture has started, and only a running capture can have a stream.
    return energy_stream_mark(&energy->stream, &CTIMER4->TC, number, label, length);
}

energy_monitor_status_t energy_monitor_accumulated(uint64_t* energy_nj, uint64_t* pairs) {
    if ((energy_nj == nullptr) || (pairs == nullptr)) {
        return ENERGY_MONITOR_BAD_ARGUMENT;
    }
    if (!dap_shared.energy_available) {
        return ENERGY_MONITOR_UNAVAILABLE;
    }
    if (!accumulator_started) {
        return capture_failed() ? ENERGY_MONITOR_DMA_ERROR : ENERGY_MONITOR_STOPPED;
    }
    uint32_t before;
    uint32_t after;
    uint64_t energy_pj;
    do {
        before = dap_shared.energy_accumulator_sequence;
        __DMB();
        energy_pj = dap_shared.energy_accumulator_pj;
        *pairs = dap_shared.energy_accumulator_pairs;
        __DMB();
        after = dap_shared.energy_accumulator_sequence;
    } while ((before != after) || ((before & 1U) != 0U));
    *energy_nj = energy_pj / 1000U;
    if (capture_failed()) {
        return ENERGY_MONITOR_DMA_ERROR;
    }
    return (*pairs == 0U) ? ENERGY_MONITOR_NO_SAMPLE : ENERGY_MONITOR_OK;
}


