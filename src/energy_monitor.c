#include "energy_monitor.h"

#include <stddef.h>

#include "fsl_clock.h"
#include "fsl_device_registers.h"
#include "fsl_iocon.h"
#include "fsl_lpadc.h"
#include "fsl_reset.h"

#include "dap_shared.h"
#include "DAP_config.h"
#include "dma_ring.h"
#include "target_sense.h"

#define ENERGY_SEGMENT_BYTES 4096U
#define ENERGY_SEGMENT_WORDS (ENERGY_SEGMENT_BYTES / sizeof(uint32_t))
#define ENERGY_SEGMENTS 1U
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
static uint32_t segments_at_start;
static bool capture_running;
static bool pins_ready;
static bool inputmux_ready;

static void pins_init(void) {
    if (pins_ready) {
        return;
    }
    CLOCK_EnableClock(kCLOCK_Iocon);
    CLOCK_EnableClock(kCLOCK_Gpio0);
    CLOCK_EnableClock(kCLOCK_Gpio1);
    IOCON_PinMuxSet(IOCON, 0U, 23U, IOCON_FUNC0 | IOCON_MODE_INACT | IOCON_ANALOG_EN | IOCON_PIO_ASW_MASK);
    IOCON_PinMuxSet(IOCON, 0U, 16U, IOCON_FUNC0 | IOCON_MODE_INACT | IOCON_ANALOG_EN | IOCON_PIO_ASW_MASK);
    IOCON_PinMuxSet(IOCON, 1U, 8U, IOCON_FUNC0 | IOCON_MODE_INACT | IOCON_ANALOG_EN | IOCON_PIO_ASW_MASK);
    pins_ready = true;
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

static void capture_start(void) {
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
    entry->link = (uint32_t)&energy_descriptors[0];

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
    LPADC_DoResetFIFO0(ADC0);
    target_sense_configure_monitor();
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
    timer_start(requested_rate_hz);
    capture_running = true;
}

void energy_monitor_pause(void) {
    if (!capture_running) {
        return;
    }
    CTIMER2->TCR = 0U;
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
    pins_init();
    if (target_sense_adc_init() != TARGET_SENSE_OK) {
        requested_rate_hz = 0U;
        actual_rate_hz = 0U;
        return ENERGY_MONITOR_ADC_ERROR;
    }
    requested_rate_hz = rate_hz;
    capture_start();
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
    uint32_t const written = ring_written() & ~((2U * sizeof(uint32_t)) - 1U);
    if (written < (2U * sizeof(uint32_t))) {
        return ENERGY_MONITOR_NO_SAMPLE;
    }
    uint32_t const written_words = written / sizeof(uint32_t);
    uint32_t const scan_words = (written_words < ENERGY_RING_WORDS) ? written_words : ENERGY_RING_WORDS;
    uint32_t current = 0U;
    uint32_t voltage = 0U;
    bool current_found = false;
    bool voltage_found = false;
    for (uint32_t offset = 0U; offset < scan_words; offset++) {
        uint32_t const index = (written_words - 1U - offset) % ENERGY_RING_WORDS;
        __DMB();
        uint32_t const result = energy_ring[index];
        if (((result & ADC_RESFIFO_VALID_MASK) == 0U) ||
            (((result & ADC_RESFIFO_TSRC_MASK) >> ADC_RESFIFO_TSRC_SHIFT) != 7U)) {
            continue;
        }
        uint32_t const command = (result >> 24U) & 0x0FU;
        if (!current_found && (command == 2U)) {
            current = result;
            current_found = true;
        } else if (current_found && (command == 1U)) {
            voltage = result;
            voltage_found = true;
            break;
        }
    }
    if (!voltage_found) {
        return ENERGY_MONITOR_NO_SAMPLE;
    }
    sample->voltage_code = (uint16_t)(voltage & 0xFFFFU);
    sample->current_code = (uint16_t)(current & 0xFFFFU);
    sample->sequence = written / (6U * sizeof(uint32_t));
    return ENERGY_MONITOR_OK;
}


