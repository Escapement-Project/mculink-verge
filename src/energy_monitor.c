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
#include "DAP_config.h"
#include "board_pins.h"
#include "dma_ring.h"
#include "target_sense.h"

#define ENERGY_SEGMENT_BYTES 4096U
#define ENERGY_SEGMENT_WORDS (ENERGY_SEGMENT_BYTES / sizeof(uint32_t))
#define ENERGY_SEGMENTS 2U
#define ENERGY_RING_WORDS (ENERGY_SEGMENT_WORDS * ENERGY_SEGMENTS)
#define ENERGY_RING_BYTES (ENERGY_RING_WORDS * sizeof(uint32_t))
#define ENERGY_DMA_SOURCE (&ADC0->RESFIFO[0])

#define ENERGY_SEGMENT_XFERCFG                                                                       \
    (DMA_CHANNEL_XFERCFG_CFGVALID_MASK | DMA_CHANNEL_XFERCFG_RELOAD_MASK |                           \
     DMA_CHANNEL_XFERCFG_SETINTA_MASK | DMA_CHANNEL_XFERCFG_WIDTH(2U) |                               \
     DMA_CHANNEL_XFERCFG_SRCINC(0U) | DMA_CHANNEL_XFERCFG_DSTINC(1U) |                                \
     DMA_CHANNEL_XFERCFG_XFERCOUNT(ENERGY_SEGMENT_WORDS - 1U))

static volatile uint32_t energy_ring[ENERGY_RING_WORDS];
DAP_SHARED_DMA_DESCRIPTOR_ALIGN static volatile dma_descriptor_t energy_descriptors[ENERGY_SEGMENTS];
static uint32_t requested_rate_hz;
static uint32_t actual_rate_hz;
static uint64_t energy_scale_q32;
static uint32_t energy_segments_at_start;
static bool capture_running;

#define ENERGY_CONTROL_POLLS 1000000U
static bool pins_ready;
static bool controls_ready;
static bool inputmux_ready;
static target_sense_current_range_t current_range;
static energy_calibration_t low_range_calibration;
static energy_calibration_t high_range_calibration;
static bool current_calibrated;
static bool accumulator_started;
static energy_sample_decoder_t sample_decoder;
static uint32_t consumed_bytes;
/** What the samples since the last DMA segment say about re-arming the low range. */
static bool on_high_range;
static bool rearm_blocked;

static_assert(ADC_RESFIFO_VALID_MASK == 0x80000000U && ADC_RESFIFO_TSRC_SHIFT == 16U,
              "energy_samples.h must match the ADC result layout");

static bool capture_failed(void) {
    if ((DMA0->COMMON[0].ERRINT & ENERGY_DMA_MASK) != 0U ||
        (capture_running && (ADC0->STAT & ADC_STAT_FOF0_MASK) != 0U)) {
        dap_shared.energy_dma_error = 1U;
    }
    return dap_shared.energy_dma_error != 0U;
}

static void accumulator_write_begin(void) {
    dap_shared.energy_accumulator_sequence++;
    __DMB();
}

static void accumulator_write_end(void) {
    __DMB();
    dap_shared.energy_accumulator_sequence++;
}

static void accumulator_reset(void) {
    accumulator_write_begin();
    dap_shared.energy_accumulator_pj = 0U;
    dap_shared.energy_accumulator_pairs = 0U;
    dap_shared.energy_accumulator_fraction_q32 = 0U;
    accumulator_write_end();
    sample_decoder = (energy_sample_decoder_t){0};
    consumed_bytes = 0U;
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

    // PRO.md, "Range switching": the amplified current on the negative input, the ladder at 28/31
    // of ACMPVREF on the positive. The output falls, and clears U18, above 90 mV on the shunt.
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
    dap_shared.energy_range_retry_active = 0U;
    dap_shared.energy_range_retry_enabled = 0U;
    dap_shared.energy_control_request = 0U;
    dap_shared.energy_control_ack = 0U;
    dap_shared.energy_segment_handler = energy_monitor_process_segment;
}

/** Out of line, so tools/energy_checks.py can skip it. */
__attribute__((noinline)) static void calibration_delay(void) {
    for (volatile uint32_t i = 0U; i < 1500000U; i++) {
        __NOP();
    }
}

/** The voltage channel halves node T (R73 and R75) on its way to VREFP's full scale. */
static uint32_t voltage_mv(uint16_t code, uint32_t full_scale_mv) {
    return ((uint32_t)code * 2U * full_scale_mv) >> 16U;
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

/** Each range's loads, smallest current first (PRO.md, "Calibration"). The divider's current alone
 *  is a point only in the low ranges: in the high ones it sits under the amplifier's output floor. */
static const calibration_load_t* const loads_350_low[] = {&no_load, &load_r1, &load_r3, nullptr};
static const calibration_load_t* const loads_350_high[] = {&load_r3, &load_r2, &load_r4, nullptr};
static const calibration_load_t* const loads_50_low[] = {&no_load, &load_r1, nullptr};
static const calibration_load_t* const loads_50_high[] = {&load_r1, &load_r3, &load_r2, nullptr};

/** 99.8% of full scale. The loads put up to 97 mV on a range that ends at 100 mV, so a unit whose
 *  tolerances add up can saturate; below this the 128 averaged conversions stay clear of 65535. */
#define CALIBRATION_CODE_LIMIT 65400U
/** Two points closer than this give no slope worth having. */
#define CALIBRATION_MIN_STEP 64U
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
    calibration_delay();
    uint16_t voltage_code = 0U;
    bool const read_ok = calibration_read_pair(&voltage_code, code);
    GPIO->CLR[1] = load->pin;
    uint32_t const full_scale_mv = target_sense_full_scale_mv();
    if (!read_ok || voltage_mv(voltage_code, full_scale_mv) < CALIBRATION_MIN_MV) {
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

/** Fills `calibration` with the points kept so far, whether or not the range succeeds. */
static bool calibration_range(const calibration_load_t* const* loads, energy_calibration_t* calibration) {
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

/** U18's Q, CTRL_HIGH_CURR, read back on P0_30: high while Q3 is off and only the low range's
 *  shunt carries the current. */
static bool latch_in_low_range(void) {
    return (GPIO->PIN[0] & (1UL << 30U)) != 0U;
}

/** PRO.md, "Calibration". */
__attribute__((noinline)) static bool current_calibrate(void) {
    current_calibrated = false;
    low_range_calibration = (energy_calibration_t){0};
    high_range_calibration = (energy_calibration_t){0};
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
    calibration_delay();
    GPIO->SET[0] = p17;
    calibration_delay();
    bool const low_ok = latch_in_low_range() &&
                        calibration_range(wide ? loads_350_low : loads_50_low, &low_range_calibration);

    GPIO->CLR[0] = p29;
    calibration_delay();
    bool const high_ok = !latch_in_low_range() &&
                         calibration_range(wide ? loads_350_high : loads_50_high, &high_range_calibration);

    GPIO->CLR[1] = CAL_DISC | loads;
    IOCON->PIO[0][29] = IOCON_FUNC7 | IOCON_SLEW_FAST | IOCON_DIGITAL_EN;
    GPIO->DIRCLR[0] = p29;
    GPIO->CLR[0] = p17;
    calibration_delay();

    current_calibrated = low_ok && high_ok;
    return current_calibrated;
}

static energy_total_t accumulator_values(void) {
    return (energy_total_t){dap_shared.energy_accumulator_pj, dap_shared.energy_accumulator_pairs,
                            dap_shared.energy_accumulator_fraction_q32};
}

static void accumulator_publish(const energy_total_t* total) {
    dap_shared.energy_accumulator_pj = total->pj;
    dap_shared.energy_accumulator_pairs = total->pairs;
    dap_shared.energy_accumulator_fraction_q32 = total->fraction_q32;
}

/** CTRL_HIGH_CURR, converted after each pair: U18's Q, high while the low range is in. */
static bool low_range(uint16_t range_code) {
    return range_code >= 32768U;
}

static energy_calibration_t const* range_calibration(bool low) {
    return low ? &low_range_calibration : &high_range_calibration;
}

/** UM11673 Table 4's low-range limits. The comparator trips above both (PRO.md, "Range
 *  switching"), so a high-range segment that stays under its setting's limit re-arms the low range. */
static uint32_t rearm_below_na(void) {
    return current_range == TARGET_SENSE_CURRENT_RANGE_350_MA ? 1500000U : 400000U;
}

static void consume_result(uint32_t result, energy_total_t* total, uint32_t full_scale_mv) {
    if (!energy_sample_feed(&sample_decoder, result) || !current_calibrated || actual_rate_hz == 0U) {
        return;
    }
    bool const low = low_range(sample_decoder.last_range);
    uint32_t const shunt_na = energy_shunt_na(sample_decoder.last_current, range_calibration(low));
    on_high_range = !low;
    if (!low && shunt_na >= rearm_below_na()) {
        rearm_blocked = true;
    }
    uint32_t const mv = voltage_mv(sample_decoder.last_voltage, full_scale_mv);
    energy_total_add(total, (uint64_t)mv * energy_target_na(shunt_na, mv), energy_scale_q32);
}

/** Only the IRQ writer or CPU1 with acknowledged quiet may advance the consumed position. */
static bool consume_until(uint32_t written) {
    uint32_t const bytes = written - consumed_bytes;
    if (bytes > ENERGY_RING_BYTES || (bytes % sizeof(uint32_t)) != 0U) {
        dap_shared.energy_dma_error = 1U;
        return false;
    }
    uint32_t const full_scale_mv = target_sense_full_scale_mv();
    accumulator_write_begin();
    energy_total_t total = accumulator_values();
    while (consumed_bytes != written) {
        uint32_t const index = (consumed_bytes / sizeof(uint32_t)) % ENERGY_RING_WORDS;
        consume_result(energy_ring[index], &total, full_scale_mv);
        consumed_bytes += sizeof(uint32_t);
    }
    accumulator_publish(&total);
    accumulator_write_end();
    return true;
}

void energy_monitor_process_segment(void) {
    if (!capture_running || actual_rate_hz == 0U || capture_failed()) {
        return;
    }
    uint32_t const completed = dap_shared.energy_segments - energy_segments_at_start + 1U;
    if (consume_until(completed * ENERGY_SEGMENT_BYTES) && on_high_range && !rearm_blocked) {
        dap_shared.energy_range_retry_pending = 1U;
    }
    rearm_blocked = false;
}

static uint32_t ring_written(void) {
    uint32_t const mask = ENERGY_DMA_MASK;
    for (;;) {
        uint32_t const sequence = dap_shared.energy_dma_sequence;
        __DMB();
        if ((sequence & 1U) != 0U) {
            continue;
        }
        uint32_t const counted = dap_shared.energy_segments - energy_segments_at_start;
        __DMB();
        bool const pending = (DMA0->COMMON[0].INTA & mask) != 0U;
        uint32_t const xfercfg = DMA0->CHANNEL[ENERGY_DMA_CHANNEL].XFERCFG;
        bool const still_pending = (DMA0->COMMON[0].INTA & mask) != 0U;
        __DMB();
        if ((pending == still_pending) && (counted == (dap_shared.energy_segments - energy_segments_at_start)) &&
            (sequence == dap_shared.energy_dma_sequence)) {
            uint32_t const left = ((xfercfg >> DMA_XFERCOUNT_SHIFT) & DMA_XFERCOUNT_MASK) + 1U;
            uint32_t const words = ((counted + (pending ? 1U : 0U)) * ENERGY_SEGMENT_WORDS) +
                                   (ENERGY_SEGMENT_WORDS - left);
            return words * sizeof(uint32_t);
        }
    }
}

static bool capture_drain(void) {
    if (!consume_until(ring_written())) {
        return false;
    }
    accumulator_write_begin();
    energy_total_t total = accumulator_values();
    bool empty = false;
    for (uint32_t i = 0U; i < 32U; i++) {
        uint32_t const result = ADC0->RESFIFO[0];
        if ((result & ADC_RESFIFO_VALID_MASK) == 0U) {
            empty = true;
            break;
        }
        consume_result(result, &total, target_sense_full_scale_mv());
    }
    // An incomplete tuple belongs to this capture, even when the next one uses the same rate.
    sample_decoder.phase = ENERGY_EXPECT_VOLTAGE;
    accumulator_publish(&total);
    accumulator_write_end();
    if (!empty) {
        dap_shared.energy_dma_error = 1U;
    }
    return empty;
}

static uint32_t timer_effective_rate(uint32_t rate_hz) {
    uint32_t const clock_hz = CLOCK_GetFroHfFreq();
    uint32_t const ticks = (clock_hz + rate_hz) / (2U * rate_hz);
    return clock_hz / (2U * ticks);
}

static void timer_start(uint32_t rate_hz) {
    uint32_t const clock_hz = CLOCK_GetFroHfFreq();
    uint32_t const ticks = (clock_hz + rate_hz) / (2U * rate_hz);
    actual_rate_hz = clock_hz / (2U * ticks);

    CLOCK_AttachClk(kFRO_HF_to_CTIMER2);
    CLOCK_EnableClock(kCLOCK_Timer2);
    RESET_PeripheralReset(kCTIMER2_RST_SHIFT_RSTn);
    CTIMER2->TCR = CTIMER_TCR_CRST_MASK;
    CTIMER2->PR = 0U;
    CTIMER2->MR[3] = ticks - 1U;
    CTIMER2->MCR = CTIMER_MCR_MR3R_MASK;
    CTIMER2->EMR = CTIMER_EMR_EMC3(3U);
    CTIMER2->IR = 0x0FU;
    CTIMER2->TCR = CTIMER_TCR_CEN_MASK;
}

__attribute__((noinline)) static void capture_ring_init(void) {
    uint32_t const source = (uint32_t)ENERGY_DMA_SOURCE;
    for (uint32_t i = 0U; i < ENERGY_SEGMENTS; i++) {
        energy_descriptors[i].xfercfg = ENERGY_SEGMENT_XFERCFG;
        energy_descriptors[i].source_end = source;
        energy_descriptors[i].destination_end =
            (uint32_t)&energy_ring[((i + 1U) * ENERGY_SEGMENT_WORDS) - 1U];
        energy_descriptors[i].link = (uint32_t)&energy_descriptors[(i + 1U) % ENERGY_SEGMENTS];
    }

    volatile dma_descriptor_t* const entry = &dap_shared.dma_table[ENERGY_DMA_CHANNEL];
    entry->xfercfg = 0U;
    entry->source_end = source;
    entry->destination_end = (uint32_t)&energy_ring[ENERGY_SEGMENT_WORDS - 1U];
    entry->link = energy_descriptors[0].link;
}

static bool capture_quiet(bool quiet) {
    uint32_t const requested = ((dap_shared.energy_control_request & ~DAP_ENERGY_QUIET_FLAG) + 2U) |
                               (quiet ? DAP_ENERGY_QUIET_FLAG : 0U);
    dap_shared.energy_control_request = requested;
    __DMB();
    MAILBOX->MBOXIRQ[DAP_CORE_MAILBOX_CPU0].IRQSET = DAP_CORE_MAILBOX_WAKE_MASK;
    for (uint32_t i = 0U; i < ENERGY_CONTROL_POLLS; i++) {
        if (dap_shared.energy_control_ack == requested) {
            __DMB();
            return true;
        }
    }
    dap_shared.energy_dma_error = 1U;
    return false;
}

__attribute__((noinline)) static bool energy_capture_start(bool new_session) {
    capture_ring_init();
    DMA0->COMMON[0].ENABLECLR = ENERGY_DMA_MASK;
    DMA0->COMMON[0].ABORT = ENERGY_DMA_MASK;
    DMA0->COMMON[0].INTENCLR = ENERGY_DMA_MASK;
    DMA0->COMMON[0].ERRINT = ENERGY_DMA_MASK;
    DMA0->COMMON[0].INTA = ENERGY_DMA_MASK;
    energy_segments_at_start = dap_shared.energy_segments;
    dap_shared.energy_range_retry_pending = 0U;
    dap_shared.energy_range_retry_enabled = 1U;
    on_high_range = false;
    rearm_blocked = false;
    sample_decoder.phase = ENERGY_EXPECT_VOLTAGE;
    consumed_bytes = 0U;
    LPADC_DoResetFIFO0(ADC0);
    actual_rate_hz = timer_effective_rate(requested_rate_hz);
    energy_scale_q32 = (UINT64_C(1) << 32U) / actual_rate_hz;
    target_sense_configure_monitor(actual_rate_hz);
    CLOCK_EnableClock(kCLOCK_InputMux);
    if (!inputmux_ready) {
        RESET_ReleasePeripheralReset(kMUX0_RST_SHIFT_RSTn);
        inputmux_ready = true;
    }
    INPUTMUX->DMA0_REQ_ENA_SET = ENERGY_DMA_MASK;
    LPADC_EnableFIFO0WatermarkDMA(ADC0, true);

    if (!capture_quiet(false)) {
        return false;
    }
    if (new_session) {
        accumulator_reset();
        accumulator_started = true;
        ADC0->STAT = ADC_STAT_FOF0_MASK;
        dap_shared.energy_dma_error = 0U;
    }
    capture_running = true;
    __DMB();
    DMA0->CHANNEL[ENERGY_DMA_CHANNEL].CFG = DMA_CHANNEL_CFG_PERIPHREQEN_MASK;
    DMA0->COMMON[0].INTENSET = ENERGY_DMA_MASK;
    DMA0->COMMON[0].ENABLESET = ENERGY_DMA_MASK;
    DMA0->CHANNEL[ENERGY_DMA_CHANNEL].XFERCFG =
        ENERGY_SEGMENT_XFERCFG | DMA_CHANNEL_XFERCFG_SWTRIG_MASK;
    // An edge whatever P0_17 was left at: CPU 0's re-arm can raise it after a pause lowered it.
    energy_latch_clock();
    timer_start(requested_rate_hz);
    return true;
}

__attribute__((noinline)) bool energy_monitor_pause(void) {
    if (!dap_shared.energy_available) {
        return true;
    }
    dap_shared.energy_range_retry_enabled = 0U;
    dap_shared.energy_range_retry_pending = 0U;
    __DMB();
    if (capture_running) {
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
    if (!capture_running) {
        return true;
    }
    if (!capture_failed() && !capture_drain()) {
        return false;
    }
    sample_decoder.phase = ENERGY_EXPECT_VOLTAGE;
    DMA0->COMMON[0].ABORT = ENERGY_DMA_MASK;
    DMA0->COMMON[0].INTA = ENERGY_DMA_MASK;
    DMA0->COMMON[0].ERRINT = ENERGY_DMA_MASK;
    INPUTMUX->DMA0_REQ_ENA_CLR = ENERGY_DMA_MASK;
    LPADC_DoResetFIFO0(ADC0);
    capture_running = false;
    return true;
}

bool energy_monitor_resume(void) {
    if ((requested_rate_hz == 0U) || capture_running) {
        return true;
    }
    if (dap_shared.energy_dma_error != 0U) {
        return false;
    }
    return energy_capture_start(false);
}

energy_monitor_status_t energy_monitor_start(uint32_t rate_hz) {
    if (!dap_shared.energy_available) {
        return ENERGY_MONITOR_UNAVAILABLE;
    }
    if ((rate_hz == 0U) || (rate_hz > ENERGY_MAX_RATE_HZ)) {
        return ENERGY_MONITOR_BAD_RATE;
    }
    if (!energy_monitor_pause()) {
        return ENERGY_MONITOR_DMA_ERROR;
    }
    requested_rate_hz = 0U;
    actual_rate_hz = 0U;
    pins_init();
    if (target_sense_adc_init() != TARGET_SENSE_OK) {
        return ENERGY_MONITOR_ADC_ERROR;
    }
    current_range = target_sense_current_range();
    if (!current_calibrate()) {
        requested_rate_hz = 0U;
        return ENERGY_MONITOR_ADC_ERROR;
    }
    requested_rate_hz = rate_hz;
    if (!energy_capture_start(true)) {
        requested_rate_hz = 0U;
        actual_rate_hz = 0U;
        return ENERGY_MONITOR_DMA_ERROR;
    }
    return ENERGY_MONITOR_OK;
}

uint32_t energy_monitor_rate(void) { return actual_rate_hz; }

energy_monitor_status_t energy_monitor_stop(void) {
    if (!energy_monitor_pause()) {
        return ENERGY_MONITOR_DMA_ERROR;
    }
    requested_rate_hz = 0U;
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
    if (!capture_running) {
        return ENERGY_MONITOR_STOPPED;
    }
    energy_sample_decoder_t decoder;
    for (;;) {
        uint32_t const before = dap_shared.energy_accumulator_sequence;
        __DMB();
        if ((before & 1U) != 0U) {
            continue;
        }
        decoder = sample_decoder;
        uint32_t const consumed = consumed_bytes;
        uint32_t const written = ring_written();
        __DMB();
        if (before != dap_shared.energy_accumulator_sequence) {
            continue;
        }
        uint32_t const bytes = written - consumed;
        if (bytes > ENERGY_RING_BYTES) {
            dap_shared.energy_dma_error = 1U;
            return ENERGY_MONITOR_DMA_ERROR;
        }
        for (uint32_t position = consumed; position != written; position += sizeof(uint32_t)) {
            uint32_t const index = (position / sizeof(uint32_t)) % ENERGY_RING_WORDS;
            (void)energy_sample_feed(&decoder, energy_ring[index]);
        }
        uint32_t const after = ring_written();
        __DMB();
        if (before != dap_shared.energy_accumulator_sequence) {
            continue;
        }
        if (after - consumed > ENERGY_RING_BYTES) {
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
    sample->voltage_code = decoder.last_voltage;
    sample->current_code = decoder.last_current;
    sample->range_code = decoder.last_range;
    sample->sequence = decoder.sequence;
    sample->voltage_mv = voltage_mv(sample->voltage_code, target_sense_full_scale_mv());
    sample->range_id = 0U;
    sample->current_ua = 0;
    sample->calibrated = false;
    if (current_range != TARGET_SENSE_CURRENT_RANGE_UNKNOWN) {
        bool const low = low_range(sample->range_code);
        sample->range_id = (current_range == TARGET_SENSE_CURRENT_RANGE_350_MA) ? (low ? 1U : 2U)
                                                                                 : (low ? 3U : 4U);
        if (current_calibrated) {
            uint32_t const target_na = energy_target_na(
                energy_shunt_na(sample->current_code, range_calibration(low)), sample->voltage_mv);
            sample->current_ua = (int32_t)(((uint64_t)target_na + 500U) / 1000U);
            sample->calibrated = true;
        }
    }
    return ENERGY_MONITOR_OK;
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


