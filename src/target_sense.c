#include "target_sense.h"

#include "fsl_clock.h"
#include "fsl_device_registers.h"
#include "fsl_iocon.h"
#include "fsl_lpadc.h"
#include "fsl_power.h"

#include "board_pins.h"
#include "core_clock.h"
#include "dap_shared.h"
#include "energy_monitor.h"

/** DBGIF_VREF: half of the debug connector's pin 1, buffered. J6 through R22/R23 on the plain
 *  board; J7 through U10 and R47/R48 on the Pro. */
#define SENSE_VREF_PORT 0U
#define SENSE_VREF_PIN 31U

/** DBGIF_DETECT: the debug connector's pin 9 (J6, or J7 on the Pro) through 2K. Nothing on the
 *  board pulls it up, so the pin's own pull-up is what makes an empty connector read high. */
#define SENSE_DETECT_PORT 0U
#define SENSE_DETECT_PIN 22U

/** Iterations of a result poll. A conversion takes tens of microseconds and an iteration a few
 *  core cycles, so this is a wait of tens of milliseconds and never the reason a read fails. */
#define SENSE_RESULT_POLLS 1000000U

/** Reads of STAT between the offset calibration finishing and the gain calibration's request. A
 *  request made within a few cycles of the offset calibration's CAL_RDY sometimes never completes:
 *  the gain results never become ready, however long the wait. 1 us apart was enough on the
 *  bench. Each read takes at least a cycle, so this is at least 10 us. */
#define SENSE_CALIBRATION_GAP_READS (10U * (CORE_CLOCK_HZ / 1000000U))

#define SENSE_COMMAND_ID 1U
#define SENSE_TRIGGER 0U
#define ENERGY_TRIGGER 7U

static bool adc_ready;

void target_sense_init(void) {
    CLOCK_EnableClock(kCLOCK_Gpio0);
    IOCON_PinMuxSet(IOCON, SENSE_VREF_PORT, SENSE_VREF_PIN,
                    IOCON_FUNC0 | IOCON_MODE_INACT | IOCON_ANALOG_EN | IOCON_PIO_ASW_MASK);
    IOCON_PinMuxSet(IOCON, SENSE_DETECT_PORT, SENSE_DETECT_PIN, IOCON_FUNC0 | IOCON_MODE_PULLUP | IOCON_DIGITAL_EN);
    GPIO->DIRCLR[SENSE_DETECT_PORT] = 1UL << SENSE_DETECT_PIN;
}

uint32_t target_sense_full_scale_mv(void) {
    return board_kind == BOARD_KIND_PRO ? 3000U : 3300U;
}

/** Bring the converter up on first use, so a probe nobody asks spends nothing on it. A Pro with
 * energy capture uses it as it boots, to calibrate.
 *
 * PLL0 clocks it rather than MAIN_CLK, so the rate does not move with the core clock: 150 MHz
 * over 8 is 18.75 MHz. The capture's timers count PLL0 itself (ENERGY_TIMER_HZ), so a pair's
 * length in their counts is exact. The reference is VREFP. The plain board ties it to VDDA, the
 * probe's own 3.3 V supply, so a reading there is only as absolute as that regulator. The Pro
 * gives it an LM4040 3.0 V reference instead (schematic sheet 2), which BOARDS.md's energy section
 * builds on.
 */
static_assert(TARGET_SENSE_ADC_DIVIDER >= 1U && TARGET_SENSE_ADC_DIVIDER <= 8U, "ADCCLKDIV divides by 1 to 8");
static_assert(PLL0_CLOCK_HZ / TARGET_SENSE_ADC_DIVIDER <= 24000000U, "fclk(ADC) is 24 MHz at most");
static target_sense_status_t adc_init(void) {
    CLOCK_SetClkDiv(kCLOCK_DivAdcAsyncClk, TARGET_SENSE_ADC_DIVIDER, true);
    CLOCK_AttachClk(kPLL0_to_ADC_CLK);
    POWER_DisablePD(kPDRUNCFG_PD_LDOGPADC);

    lpadc_config_t config;
    LPADC_GetDefaultConfig(&config);
    config.enableAnalogPreliminary = true;
    // CFG[REFSEL] 2, VREFP (UM11126 Table 720). Alt2 is 1, VDDA.
    config.referenceVoltageSource = kLPADC_ReferenceVoltageAlt3;
    config.conversionAverageMode = kLPADC_ConversionAverage128;
    LPADC_Init(ADC0, &config);

    // Both return kStatus_Timeout rather than spinning, because CMakeLists.txt sets the driver's
    // timeout options. Without them these are unbounded waits on a status bit.
    if (LPADC_DoOffsetCalibration(ADC0) != kStatus_Success) {
        return TARGET_SENSE_OFFSET_CALIBRATION;
    }
    for (uint32_t i = 0U; i < SENSE_CALIBRATION_GAP_READS; i++) {
        (void)ADC0->STAT;
    }
    // DoAutoCalibration is the same two calls with the status thrown away.
    LPADC_PrepareAutoCalibration(ADC0);
    if (LPADC_FinishAutoCalibration(ADC0) != kStatus_Success) {
        return TARGET_SENSE_GAIN_CALIBRATION;
    }

    lpadc_conv_trigger_config_t trigger;
    LPADC_GetDefaultConvTriggerConfig(&trigger);
    trigger.targetCommandId = SENSE_COMMAND_ID;
    trigger.enableHardwareTrigger = false;
    LPADC_SetConvTriggerConfig(ADC0, SENSE_TRIGGER, &trigger);
    return TARGET_SENSE_OK;
}

target_sense_status_t target_sense_adc_init(void) {
    if (adc_ready) {
        return TARGET_SENSE_OK;
    }
    target_sense_status_t const status = adc_init();
    adc_ready = status == TARGET_SENSE_OK;
    return status;
}

target_sense_current_range_t target_sense_current_range(void) {
    uint16_t code = 0U;
    if (target_sense_convert(4U, false, &code) != TARGET_SENSE_OK) {
        return TARGET_SENSE_CURRENT_RANGE_UNKNOWN;
    }
    uint32_t high_count = 0U;
    for (uint32_t i = 0U; i < 3U; i++) {
        if (target_sense_convert(4U, false, &code) != TARGET_SENSE_OK) {
            return TARGET_SENSE_CURRENT_RANGE_UNKNOWN;
        }
        high_count += code >= 32768U ? 1U : 0U;
    }
    return high_count >= 2U ? TARGET_SENSE_CURRENT_RANGE_350_MA : TARGET_SENSE_CURRENT_RANGE_50_MA;
}

static lpadc_hardware_average_mode_t monitor_average_mode(uint32_t rate_hz) {
    // A pair at N averages takes 27 + 32 N ADCK cycles, the unaveraged range-state conversion
    // included. Each limit is the fastest effective rate whose period holds one at every request
    // that rounds to it: 9,037 can come from a period a quarter cycle short of 64 averages. A
    // longer sample time in configure_pair_command moves them. 4 averages fit at
    // ENERGY_MAX_RATE_HZ.
    if (rate_hz <= 4546U) {
        return kLPADC_HardwareAverageCount128;
    }
    if (rate_hz <= 9036U) {
        return kLPADC_HardwareAverageCount64;
    }
    if (rate_hz <= 17840U) {
        return kLPADC_HardwareAverageCount32;
    }
    if (rate_hz <= 34786U) {
        return kLPADC_HardwareAverageCount16;
    }
    if (rate_hz <= 66254U) {
        return kLPADC_HardwareAverageCount8;
    }
    return kLPADC_HardwareAverageCount4;
}

/** A conversion takes 32 ADC clocks at ADCK11, so N averages take 32 N; the range's is the rest. */
uint32_t target_sense_monitor_window_adck(uint32_t rate_hz) {
    return 32U << (uint32_t)monitor_average_mode(rate_hz);
}

uint32_t target_sense_monitor_pair_adck(uint32_t rate_hz) {
    return 27U + target_sense_monitor_window_adck(rate_hz);
}

/** The capture's pair as command 1, chained to `next`: the voltage on CH0A (PIO0_23/ADC0_0) and the
 *  current on CH0B (PIO0_16/ADC0_8) in one dual single-ended conversion, so both average over the
 *  same window. With both sides sent to FIFO0, the A side's result always comes first (UM11126
 *  section 38.7.8). Nothing else in a result tells them apart, so the trigger keeps the B side in
 *  FIFO0 and energy_sample_feed reads them in that order. */
static void configure_pair_command(lpadc_hardware_average_mode_t average, uint32_t next,
                                   lpadc_conv_command_config_t* command) {
    LPADC_GetDefaultConvCommandConfig(command);
    command->channelNumber = 0U;
    command->sampleChannelMode = kLPADC_SampleChannelDualSingleEndBothSide;
    command->conversionResolutionMode = kLPADC_ConversionResolutionHigh;
    command->hardwareAverageMode = average;
    // 11.5 cycles, 613 ns. With a 12 MHz ADC clock, 292 ns read the voltage 0.27% higher
    // unaveraged than averaged 16 times, and 625 ns read every averaging within 0.02% of 16 times
    // at 10,000 pairs/s.
    command->sampleTimeMode = kLPADC_SampleTimeADCK11;
    command->chainedNextCommandNumber = next;
    LPADC_SetConvCommandConfig(ADC0, 1U, command);
}

void target_sense_configure_monitor(uint32_t rate_hz) {
    lpadc_conv_command_config_t command;
    configure_pair_command(monitor_average_mode(rate_hz), 2U, &command);

    command.channelNumber = 2U;
    command.sampleChannelMode = kLPADC_SampleChannelSingleEndSideA;
    // Keep the range-state conversion fresh so gain changes appear in the next captured pair.
    command.hardwareAverageMode = kLPADC_HardwareAverageCount1;
    command.sampleTimeMode = kLPADC_SampleTimeADCK3;
    command.chainedNextCommandNumber = 0U;
    LPADC_SetConvCommandConfig(ADC0, 2U, &command);

    lpadc_conv_trigger_config_t trigger;
    LPADC_GetDefaultConvTriggerConfig(&trigger);
    trigger.targetCommandId = 1U;
    trigger.enableHardwareTrigger = true;
    trigger.channelBFIFOSelect = 0U;
    LPADC_SetConvTriggerConfig(ADC0, ENERGY_TRIGGER, &trigger);
}

static target_sense_status_t target_sense_convert_sample(uint8_t channel, bool side_b, uint16_t* code,
                                                          lpadc_hardware_average_mode_t average_mode) {
    // A capture that failed before this read stays failed, and resume refuses it: 0x90 reports
    // that as result 7. Only a resume that fails now fails the read.
    bool const failed_before = dap_shared.energy_dma_error != 0U;
    if (!energy_monitor_pause()) {
        return TARGET_SENSE_NO_RESULT;
    }
    target_sense_status_t status = target_sense_adc_init();
    if (status != TARGET_SENSE_OK) {
        (void)energy_monitor_resume();
        return status;
    }

    LPADC_DoResetFIFO0(ADC0);
    lpadc_conv_command_config_t command;
    LPADC_GetDefaultConvCommandConfig(&command);
    command.channelNumber = channel;
    command.sampleChannelMode = side_b ? kLPADC_SampleChannelSingleEndSideB : kLPADC_SampleChannelSingleEndSideA;
    command.conversionResolutionMode = kLPADC_ConversionResolutionHigh;
    command.hardwareAverageMode = average_mode;
    command.sampleTimeMode = kLPADC_SampleTimeADCK35;
    LPADC_SetConvCommandConfig(ADC0, SENSE_COMMAND_ID, &command);

    lpadc_conv_trigger_config_t trigger;
    LPADC_GetDefaultConvTriggerConfig(&trigger);
    trigger.targetCommandId = SENSE_COMMAND_ID;
    trigger.enableHardwareTrigger = false;
    LPADC_SetConvTriggerConfig(ADC0, SENSE_TRIGGER, &trigger);

    LPADC_DoSoftwareTrigger(ADC0, 1UL << SENSE_TRIGGER);
    lpadc_conv_result_t result;
    for (uint32_t i = 0U; i < SENSE_RESULT_POLLS; i++) {
        if (LPADC_GetConvResult(ADC0, &result, 0U)) {
            *code = result.convValue;
            status = TARGET_SENSE_OK;
            return (energy_monitor_resume() || failed_before) ? status : TARGET_SENSE_NO_RESULT;
        }
    }
    (void)energy_monitor_resume();
    return TARGET_SENSE_NO_RESULT;
}

target_sense_status_t target_sense_convert(uint8_t channel, bool side_b, uint16_t* code) {
    return target_sense_convert_sample(channel, side_b, code, kLPADC_HardwareAverageCount16);
}

target_sense_status_t target_sense_convert_pair_for_calibration(uint16_t* voltage_code, uint16_t* current_code) {
    if ((voltage_code == nullptr) || (current_code == nullptr)) {
        return TARGET_SENSE_NO_RESULT;
    }
    if (!energy_monitor_pause()) {
        return TARGET_SENSE_NO_RESULT;
    }
    target_sense_status_t status = target_sense_adc_init();
    if (status != TARGET_SENSE_OK) {
        (void)energy_monitor_resume();
        return status;
    }

    LPADC_DoResetFIFO0(ADC0);
    lpadc_conv_command_config_t command;
    // The capture's sample time with the most averaging: averaging leaves the transfer alone and
    // takes the noise out of the points every later reading is scaled by.
    configure_pair_command(kLPADC_HardwareAverageCount128, 0U, &command);

    lpadc_conv_trigger_config_t trigger;
    LPADC_GetDefaultConvTriggerConfig(&trigger);
    trigger.targetCommandId = 1U;
    trigger.enableHardwareTrigger = false;
    trigger.channelBFIFOSelect = 0U;
    LPADC_SetConvTriggerConfig(ADC0, SENSE_TRIGGER, &trigger);
    LPADC_DoSoftwareTrigger(ADC0, 1UL << SENSE_TRIGGER);

    // The voltage's result, then the current's (configure_pair_command).
    uint32_t found = 0U;
    lpadc_conv_result_t result;
    for (uint32_t i = 0U; i < SENSE_RESULT_POLLS && found < 2U; i++) {
        if (!LPADC_GetConvResult(ADC0, &result, 0U) || (result.commandIdSource != 1U)) {
            continue;
        }
        *((found == 0U) ? voltage_code : current_code) = result.convValue;
        found++;
    }
    status = (found == 2U) ? TARGET_SENSE_OK : TARGET_SENSE_NO_RESULT;
    return energy_monitor_resume() ? status : TARGET_SENSE_NO_RESULT;
}

bool target_sense_detected(void) {
    return GPIO->B[SENSE_DETECT_PORT][SENSE_DETECT_PIN] == 0U;
}
