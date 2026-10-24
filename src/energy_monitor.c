#include "energy_monitor.h"

#include <stddef.h>

#include "fsl_clock.h"
#include "fsl_device_registers.h"
#include "fsl_iocon.h"
#include "fsl_lpadc.h"
#include "fsl_power.h"
#include "fsl_reset.h"

#include "dap_shared.h"
#include "DAP_config.h"
#include "board_pins.h"
#include "dma_ring.h"
#include "target_sense.h"

#define ENERGY_SEGMENT_BYTES 4096U
#define ENERGY_SEGMENT_WORDS (ENERGY_SEGMENT_BYTES / sizeof(uint32_t))
#define ENERGY_PAIR_WORDS 9U
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
static volatile dma_descriptor_t energy_descriptors[ENERGY_SEGMENTS]
    DAP_SHARED_DMA_DESCRIPTOR_ALIGN;
static uint32_t requested_rate_hz;
static uint32_t actual_rate_hz;
static uint64_t energy_scale_q32;
static uint32_t segments_at_start;
static bool capture_running;
static bool pins_ready;
static bool controls_ready;
static bool inputmux_ready;
static target_sense_current_range_t current_range;
typedef struct {
    uint16_t code[4];
    uint32_t current_ua[4];
    uint32_t slope_ua_per_code_q16[3];
    uint8_t count;
} current_calibration_t;
static current_calibration_t current_calibration_high;
static current_calibration_t current_calibration_low;
static bool current_calibrated;
static bool accumulator_started;
static uint8_t sample_phase;
static uint16_t sample_voltage_code;
static uint16_t sample_current_code;

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
    dap_shared.energy_accumulator_nj = 0U;
    dap_shared.energy_accumulator_pairs = 0U;
    dap_shared.energy_accumulator_fraction_q32 = 0U;
    accumulator_write_end();
    sample_phase = 0U;
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
    IOCON_PinMuxSet(IOCON, 0U, 17U, IOCON_FUNC0 | IOCON_MODE_INACT | IOCON_DIGITAL_EN);
    IOCON_PinMuxSet(IOCON, 0U, 29U, IOCON_FUNC7 | IOCON_SLEW_FAST | IOCON_DIGITAL_EN);
    IOCON_PinMuxSet(IOCON, 0U, 30U, IOCON_FUNC0 | IOCON_MODE_INACT | IOCON_DIGITAL_EN);

    POWER_DisablePD(kPDRUNCFG_PD_COMP);
    PMC->COMP = PMC_COMP_NMUX(4U) | PMC_COMP_VREF(28U);
    pins_ready = true;
}

void energy_monitor_init(void) {
    controls_init();
    pins_init();
    dap_shared.energy_accumulator_sequence = 0U;
    dap_shared.energy_accumulator_nj = 0U;
    dap_shared.energy_accumulator_pairs = 0U;
    dap_shared.energy_accumulator_fraction_q32 = 0U;
    dap_shared.energy_range_retry_pending = 0U;
    dap_shared.energy_range_retry_active = 0U;
    dap_shared.energy_range_retry_enabled = 0U;
    dap_shared.energy_segment_handler = energy_monitor_process_segment;
}

static void calibration_delay(void) {
    for (volatile uint32_t i = 0U; i < 1500000U; i++) {
        __NOP();
    }
}

static bool calibration_read_pair(uint32_t rate_hz, uint16_t* voltage_code, uint16_t* current_code) {
    uint32_t voltage_sum = 0U;
    uint32_t current_sum = 0U;
    for (uint32_t i = 0U; i < 4U; i++) {
        uint16_t voltage = 0U;
        uint16_t current = 0U;
        if (target_sense_convert_pair_for_calibration(rate_hz, &voltage, &current) != TARGET_SENSE_OK) {
            return false;
        }
        voltage_sum += voltage;
        current_sum += current;
    }
    *voltage_code = (uint16_t)(voltage_sum / 4U);
    *current_code = (uint16_t)(current_sum / 4U);
    return true;
}

static bool calibration_point(uint32_t rate_hz, uint32_t resistor_pin, uint32_t resistor_milliohms,
                              uint16_t* code, uint32_t* current_ua) {
    uint16_t voltage_code = 0U;
    GPIO->SET[1] = resistor_pin;
    calibration_delay();
    bool const read_ok = calibration_read_pair(rate_hz, &voltage_code, code);
    GPIO->CLR[1] = resistor_pin;
    if (!read_ok || (voltage_code < 10000U)) {
        return false;
    }
    uint32_t const voltage_mv = ((uint32_t)voltage_code * 6600U) / 65536U;
    if ((voltage_mv == 0U) || (resistor_milliohms == 0U)) {
        return false;
    }
    uint64_t const current = ((uint64_t)voltage_mv * 1000000U) / resistor_milliohms;
    if ((current == 0U) || (current > UINT32_MAX)) {
        return false;
    }
    *current_ua = (uint32_t)current;
    return true;
}

static bool calibration_range(uint32_t rate_hz, current_calibration_t* calibration, uint32_t reset_release,
                              uint32_t first_resistor, uint32_t first_resistor_milliohms,
                              uint32_t second_resistor, uint32_t second_resistor_milliohms,
                              uint32_t third_resistor, uint32_t third_resistor_milliohms) {
    uint32_t const p30 = 1UL << 30U;
    bool const high = reset_release != 0U;
    bool const state_matches = ((GPIO->PIN[0] & p30) != 0U) == high;
    uint16_t offset_voltage = 0U;
    uint16_t offset_code = 0U;
    if (!state_matches || !calibration_read_pair(rate_hz, &offset_voltage, &offset_code)) {
        return false;
    }
    calibration->code[0] = offset_code;
    calibration->current_ua[0] = 0U;
    calibration->count = 1U;
    uint32_t const resistors[] = {first_resistor, second_resistor, third_resistor};
    uint32_t const resistances[] = {first_resistor_milliohms, second_resistor_milliohms,
                                    third_resistor_milliohms};
    for (uint32_t i = 0U; i < 3U; i++) {
        if (resistors[i] == 0U) {
            break;
        }
        uint32_t const point = calibration->count;
        if (!calibration_point(rate_hz, resistors[i], resistances[i], &calibration->code[point],
                               &calibration->current_ua[point])) {
            return false;
        }
        if ((calibration->code[point] <= calibration->code[point - 1U]) ||
            (calibration->current_ua[point] <= calibration->current_ua[point - 1U])) {
            return false;
        }
        calibration->count++;
    }
    return calibration->count >= 2U;
}

static bool calibration_high_range(uint32_t rate_hz, current_calibration_t* calibration, uint32_t reset_release,
                                   uint32_t resistor, uint32_t resistor_milliohms) {
    uint32_t const p30 = 1UL << 30U;
    uint16_t voltage = 0U;
    uint16_t offset = 0U;
    uint16_t loaded = 0U;
    uint32_t current_ua = 0U;
    bool const state_matches = ((GPIO->PIN[0] & p30) != 0U) == (reset_release != 0U);
    if (!state_matches || !calibration_read_pair(rate_hz, &voltage, &offset) ||
        !calibration_point(rate_hz, resistor, resistor_milliohms, &loaded, &current_ua) || (loaded <= offset)) {
        return false;
    }
    calibration->code[0] = offset;
    calibration->code[1] = loaded;
    calibration->current_ua[0] = 0U;
    calibration->current_ua[1] = current_ua;
    calibration->count = 2U;
    return true;
}

static void calibration_prepare(current_calibration_t* calibration) {
    for (uint32_t i = 0U; (i + 1U) < calibration->count; i++) {
        uint64_t const current_delta = calibration->current_ua[i + 1U] - calibration->current_ua[i];
        uint32_t const code_delta = calibration->code[i + 1U] - calibration->code[i];
        calibration->slope_ua_per_code_q16[i] = (uint32_t)((current_delta << 16U) / code_delta);
    }
}

static bool current_calibrate(uint32_t rate_hz) {
    current_calibrated = false;
    if (current_range == TARGET_SENSE_CURRENT_RANGE_UNKNOWN) {
        return false;
    }

    uint32_t const p29 = 1UL << 29U;
    uint32_t const p17 = 1UL << 17U;
    uint32_t const cal_disc = 1UL << 24U;
    uint32_t const cal_r1 = 1UL << 22U;
    uint32_t const cal_r2 = 1UL << 23U;
    uint32_t const cal_r3 = 1UL << 26U;
    uint32_t const cal_r4 = 1UL << 29U;
    bool high_ok = false;
    bool low_ok = false;
    current_calibration_high = (current_calibration_t){0};
    current_calibration_low = (current_calibration_t){0};

    GPIO->CLR[1] = cal_r1 | cal_r2 | cal_r3 | cal_r4;
    GPIO->SET[1] = cal_disc;
    GPIO->SET[0] = p29;
    GPIO->DIRSET[0] = p29;
    IOCON->PIO[0][29] = IOCON_FUNC0 | IOCON_MODE_INACT | IOCON_DIGITAL_EN;
    GPIO->CLR[0] = p17;
    calibration_delay();
    GPIO->SET[0] = p17;
    calibration_delay();
    high_ok = calibration_high_range(rate_hz, &current_calibration_high, p29, cal_r1, 6650000U);

    GPIO->CLR[0] = p29;
    calibration_delay();
    if (current_range == TARGET_SENSE_CURRENT_RANGE_350_MA) {
        low_ok = calibration_range(rate_hz, &current_calibration_low, 0U, cal_r1, 6650000U,
                                   cal_r3, 1650000U, cal_r4, 16500U);
    } else {
        low_ok = calibration_range(rate_hz, &current_calibration_low, 0U, cal_r1, 6650000U,
                                   cal_r3, 1650000U, cal_r2, 66500U);
    }

    GPIO->CLR[1] = cal_disc | cal_r1 | cal_r2 | cal_r3 | cal_r4;
    IOCON->PIO[0][29] = IOCON_FUNC7 | IOCON_SLEW_FAST | IOCON_DIGITAL_EN;
    GPIO->DIRCLR[0] = p29;
    GPIO->CLR[0] = p17;
    calibration_delay();

    current_calibrated = high_ok && low_ok;
    if (high_ok) {
        calibration_prepare(&current_calibration_high);
    }
    if (low_ok) {
        calibration_prepare(&current_calibration_low);
    }
    return current_calibrated;
}

static int32_t current_calculate(uint16_t code, current_calibration_t const* calibration) {
    uint32_t segment = 0U;
    while (((segment + 2U) < calibration->count) && (code >= calibration->code[segment + 1U])) {
        segment++;
    }
    int64_t const code_delta = (int32_t)code - calibration->code[segment];
    int64_t const correction =
        (code_delta * calibration->slope_ua_per_code_q16[segment]) / 65536;
    int64_t const current = calibration->current_ua[segment] + correction;
    if (current <= 0) {
        return 0;
    }
    return current > INT32_MAX ? INT32_MAX : (int32_t)current;
}

static void accumulate_sample(uint16_t voltage_code, uint16_t current_code, uint16_t range_code,
                              uint64_t* energy_nj, uint64_t* pairs, uint32_t* fraction_q32) {
    if (!current_calibrated || (actual_rate_hz == 0U)) {
        return;
    }
    bool const high_gain = range_code >= 32768U;
    current_calibration_t const* const calibration =
        high_gain ? &current_calibration_high : &current_calibration_low;
    int32_t const current_ua = current_calculate(current_code, calibration);
    uint32_t const voltage_mv = ((uint32_t)voltage_code * 6600U) / 65536U;
    uint64_t const power_nw = (uint64_t)voltage_mv * (uint32_t)current_ua;

    uint64_t const scaled_nj_q32 = power_nw * energy_scale_q32;
    uint64_t increment = scaled_nj_q32 >> 32U;
    uint32_t const previous_fraction = *fraction_q32;
    *fraction_q32 += (uint32_t)scaled_nj_q32;
    increment += *fraction_q32 < previous_fraction;
    *energy_nj = (UINT64_MAX - *energy_nj < increment) ? UINT64_MAX : *energy_nj + increment;
    if (*pairs != UINT64_MAX) {
        (*pairs)++;
    }
}

void energy_monitor_process_segment(void) {
    if (!capture_running || (actual_rate_hz == 0U)) {
        return;
    }
    uint32_t const completed = dap_shared.energy_segments - segments_at_start;
    uint32_t const segment = completed % ENERGY_SEGMENTS;
    uint32_t const first_word = segment * ENERGY_SEGMENT_WORDS;
    uint64_t energy_nj = dap_shared.energy_accumulator_nj;
    uint64_t pairs = dap_shared.energy_accumulator_pairs;
    uint32_t fraction_q32 = dap_shared.energy_accumulator_fraction_q32;
    uint64_t const initial_pairs = pairs;
    for (uint32_t offset = 0U; offset < ENERGY_SEGMENT_WORDS; offset++) {
        uint32_t const result = energy_ring[first_word + offset];
        if (((result & ADC_RESFIFO_VALID_MASK) == 0U) ||
            (((result & ADC_RESFIFO_TSRC_MASK) >> ADC_RESFIFO_TSRC_SHIFT) != 7U)) {
            continue;
        }
        uint32_t const command = (result >> 24U) & 0x0FU;
        if (command == 1U) {
            sample_voltage_code = (uint16_t)(result & 0xFFFFU);
            sample_phase = 1U;
        } else if ((command == 2U) && (sample_phase == 1U)) {
            sample_current_code = (uint16_t)(result & 0xFFFFU);
            sample_phase = 2U;
        } else if ((command == 3U) && (sample_phase == 2U)) {
            accumulate_sample(sample_voltage_code, sample_current_code,
                              (uint16_t)(result & 0xFFFFU), &energy_nj, &pairs, &fraction_q32);
            sample_phase = 0U;
        }
    }
    if (pairs != initial_pairs) {
        accumulator_write_begin();
        dap_shared.energy_accumulator_nj = energy_nj;
        dap_shared.energy_accumulator_pairs = pairs;
        dap_shared.energy_accumulator_fraction_q32 = fraction_q32;
        accumulator_write_end();
    }
}

static uint32_t ring_written(void) {
    uint32_t const mask = ENERGY_DMA_MASK;
    for (;;) {
        uint32_t const counted = dap_shared.energy_segments - segments_at_start;
        __DMB();
        bool const pending = (DMA0->COMMON[0].INTA & mask) != 0U;
        uint32_t const xfercfg = DMA0->CHANNEL[ENERGY_DMA_CHANNEL].XFERCFG;
        bool const still_pending = (DMA0->COMMON[0].INTA & mask) != 0U;
        __DMB();
        if ((pending == still_pending) && (counted == (dap_shared.energy_segments - segments_at_start))) {
            uint32_t const left = ((xfercfg >> DMA_XFERCOUNT_SHIFT) & DMA_XFERCOUNT_MASK) + 1U;
            uint32_t const words = ((counted + (pending ? 1U : 0U)) * ENERGY_SEGMENT_WORDS) +
                                   (ENERGY_SEGMENT_WORDS - left);
            return words * sizeof(uint32_t);
        }
    }
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

static void capture_start(void) {
    capture_ring_init();
    DMA0->COMMON[0].ENABLECLR = ENERGY_DMA_MASK;
    DMA0->COMMON[0].ABORT = ENERGY_DMA_MASK;
    DMA0->COMMON[0].INTENCLR = ENERGY_DMA_MASK;
    DMA0->COMMON[0].ERRINT = ENERGY_DMA_MASK;
    DMA0->COMMON[0].INTA = ENERGY_DMA_MASK;
    uint32_t const cleared = TIMESTAMP_GET();
    for (uint32_t i = 0U; (i < 10000U) &&
                          ((TIMESTAMP_GET() - cleared) < (2U * (TIMESTAMP_CLOCK / 1000000U)));
         i++) {
    }
    segments_at_start = dap_shared.energy_segments;
    dap_shared.energy_dma_error = 0U;
    dap_shared.energy_range_retry_pending = 0U;
    dap_shared.energy_range_retry_enabled = 1U;
    sample_phase = 0U;
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

    __DMB();
    DMA0->CHANNEL[ENERGY_DMA_CHANNEL].CFG = DMA_CHANNEL_CFG_PERIPHREQEN_MASK;
    DMA0->COMMON[0].INTENSET = ENERGY_DMA_MASK;
    DMA0->COMMON[0].ENABLESET = ENERGY_DMA_MASK;
    DMA0->CHANNEL[ENERGY_DMA_CHANNEL].XFERCFG =
        ENERGY_SEGMENT_XFERCFG | DMA_CHANNEL_XFERCFG_SWTRIG_MASK;
    GPIO->SET[0] = 1UL << 17U;
    timer_start(requested_rate_hz);
    capture_running = true;
}

void energy_monitor_pause(void) {
    dap_shared.energy_range_retry_enabled = 0U;
    dap_shared.energy_range_retry_pending = 0U;
    __DMB();
    while (dap_shared.energy_range_retry_active != 0U) {
    }
    if (!capture_running) {
        return;
    }
    CTIMER2->TCR = 0U;
    GPIO->CLR[0] = 1UL << 17U;
    LPADC_EnableFIFO0WatermarkDMA(ADC0, false);
    DMA0->COMMON[0].ENABLECLR = ENERGY_DMA_MASK;
    for (uint32_t i = 0U; (i < 1000U) && ((DMA0->COMMON[0].BUSY & ENERGY_DMA_MASK) != 0U); i++) {
    }
    DMA0->COMMON[0].ABORT = ENERGY_DMA_MASK;
    DMA0->COMMON[0].INTENCLR = ENERGY_DMA_MASK;
    DMA0->COMMON[0].INTA = ENERGY_DMA_MASK;
    INPUTMUX->DMA0_REQ_ENA_CLR = ENERGY_DMA_MASK;
    LPADC_DoResetFIFO0(ADC0);
    capture_running = false;
}

void energy_monitor_resume(void) {
    if ((requested_rate_hz != 0U) && !capture_running) {
        capture_start();
    }
}

energy_monitor_status_t energy_monitor_start(uint32_t rate_hz) {
    if (!dap_shared.energy_available) {
        return ENERGY_MONITOR_UNAVAILABLE;
    }
    if ((rate_hz == 0U) || (rate_hz > ENERGY_MAX_RATE_HZ)) {
        return ENERGY_MONITOR_BAD_RATE;
    }
    energy_monitor_pause();
    requested_rate_hz = 0U;
    actual_rate_hz = 0U;
    pins_init();
    if (target_sense_adc_init() != TARGET_SENSE_OK) {
        return ENERGY_MONITOR_ADC_ERROR;
    }
    current_range = target_sense_current_range();
    if (!current_calibrate(rate_hz)) {
        requested_rate_hz = 0U;
        return ENERGY_MONITOR_ADC_ERROR;
    }
    accumulator_reset();
    requested_rate_hz = rate_hz;
    capture_start();
    accumulator_started = true;
    return ENERGY_MONITOR_OK;
}

uint32_t energy_monitor_rate(void) { return actual_rate_hz; }

void energy_monitor_stop(void) {
    energy_monitor_pause();
    requested_rate_hz = 0U;
    actual_rate_hz = 0U;
    CLOCK_DisableClock(kCLOCK_Timer2);
    CLOCK_AttachClk(kNONE_to_CTIMER2);
}

energy_monitor_status_t energy_monitor_latest(energy_monitor_sample_t* sample) {
    if (sample == NULL) {
        return ENERGY_MONITOR_BAD_ARGUMENT;
    }
    if (!dap_shared.energy_available) {
        return ENERGY_MONITOR_UNAVAILABLE;
    }
    if (!capture_running) {
        return ENERGY_MONITOR_STOPPED;
    }
    if (dap_shared.energy_dma_error != 0U) {
        return ENERGY_MONITOR_DMA_ERROR;
    }
    uint32_t const pair_bytes = ENERGY_PAIR_WORDS * sizeof(uint32_t);
    uint32_t const written = ring_written();
    if (written < pair_bytes) {
        return ENERGY_MONITOR_NO_SAMPLE;
    }
    uint32_t const written_words = written / sizeof(uint32_t);
    uint32_t const scan_words = (written_words < ENERGY_RING_WORDS) ? written_words : ENERGY_RING_WORDS;
    uint32_t current = 0U;
    uint32_t voltage = 0U;
    uint32_t range = 0U;
    bool current_found = false;
    bool voltage_found = false;
    bool range_found = false;
    for (uint32_t offset = 0U; offset < scan_words; offset++) {
        uint32_t const index = (written_words - 1U - offset) % ENERGY_RING_WORDS;
        __DMB();
        uint32_t const result = energy_ring[index];
        if (((result & ADC_RESFIFO_VALID_MASK) == 0U) ||
            (((result & ADC_RESFIFO_TSRC_MASK) >> ADC_RESFIFO_TSRC_SHIFT) != 7U)) {
            continue;
        }
        uint32_t const command = (result >> 24U) & 0x0FU;
        if (!range_found && (command == 3U)) {
            range = result;
            range_found = true;
        } else if (range_found && !current_found && (command == 2U)) {
            current = result;
            current_found = true;
        } else if (current_found && (command == 1U)) {
            voltage = result;
            voltage_found = true;
            break;
        }
    }
    if (!current_found || !voltage_found || !range_found) {
        return ENERGY_MONITOR_NO_SAMPLE;
    }
    sample->voltage_code = (uint16_t)(voltage & 0xFFFFU);
    sample->current_code = (uint16_t)(current & 0xFFFFU);
    sample->range_code = (uint16_t)(range & 0xFFFFU);
    sample->sequence = written / pair_bytes;
    sample->voltage_mv = ((uint32_t)sample->voltage_code * 6600U) / 65536U;
    sample->range_id = 0U;
    sample->current_ua = 0;
    sample->calibrated = false;
    if (current_range != TARGET_SENSE_CURRENT_RANGE_UNKNOWN) {
        bool const high_gain = sample->range_code >= 32768U;
        sample->range_id = (current_range == TARGET_SENSE_CURRENT_RANGE_350_MA)
                               ? (high_gain ? 1U : 2U)
                               : (high_gain ? 3U : 4U);
        if (current_calibrated) {
            current_calibration_t const* const calibration =
                high_gain ? &current_calibration_high : &current_calibration_low;
            sample->current_ua = current_calculate(sample->current_code, calibration);
            sample->calibrated = true;
        }
    }
    return ENERGY_MONITOR_OK;
}

energy_monitor_status_t energy_monitor_accumulated(uint64_t* energy_nj, uint64_t* pairs) {
    if ((energy_nj == NULL) || (pairs == NULL)) {
        return ENERGY_MONITOR_BAD_ARGUMENT;
    }
    if (!dap_shared.energy_available) {
        return ENERGY_MONITOR_UNAVAILABLE;
    }
    if (!accumulator_started) {
        return ENERGY_MONITOR_STOPPED;
    }
    uint32_t before;
    uint32_t after;
    do {
        before = dap_shared.energy_accumulator_sequence;
        __DMB();
        *energy_nj = dap_shared.energy_accumulator_nj;
        *pairs = dap_shared.energy_accumulator_pairs;
        __DMB();
        after = dap_shared.energy_accumulator_sequence;
    } while ((before != after) || ((before & 1U) != 0U));
    return (*pairs == 0U) ? ENERGY_MONITOR_NO_SAMPLE : ENERGY_MONITOR_OK;
}


