#include "target_sense.h"

#include "fsl_clock.h"
#include "fsl_device_registers.h"
#include "fsl_iocon.h"
#include "fsl_lpadc.h"
#include "fsl_power.h"

#include "core_clock.h"

/** DBGIF_VREF: half of the buffered J6 pin 1, through R22/R23. */
#define SENSE_VREF_PORT 0U
#define SENSE_VREF_PIN 31U

/** DBGIF_DETECT: J6 pin 9 through 2K. Nothing on the board pulls it up, so the pin's own pull-up
 *  is what makes an empty connector read high. */
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

static bool adc_ready;

void target_sense_init(void) {
    CLOCK_EnableClock(kCLOCK_Gpio0);
    IOCON_PinMuxSet(IOCON, SENSE_VREF_PORT, SENSE_VREF_PIN,
                    IOCON_FUNC0 | IOCON_MODE_INACT | IOCON_ANALOG_EN | IOCON_PIO_ASW_MASK);
    IOCON_PinMuxSet(IOCON, SENSE_DETECT_PORT, SENSE_DETECT_PIN, IOCON_FUNC0 | IOCON_MODE_PULLUP | IOCON_DIGITAL_EN);
    GPIO->DIRCLR[SENSE_DETECT_PORT] = 1UL << SENSE_DETECT_PIN;
}

/** Bring the converter up on first use, so a probe nobody asks spends nothing on it.
 *
 * FRO_HF clocks it rather than MAIN_CLK, so the rate does not move with the core clock: 96 MHz
 * over 8 is 12 MHz. VDDA and VREFP are tied together on this board, so either reference is the
 * probe's own supply, and a reading is only as absolute as that regulator.
 */
static target_sense_status_t adc_init(void) {
    CLOCK_SetClkDiv(kCLOCK_DivAdcAsyncClk, 8U, true);
    CLOCK_AttachClk(kFRO_HF_to_ADC_CLK);
    POWER_DisablePD(kPDRUNCFG_PD_LDOGPADC);

    lpadc_config_t config;
    LPADC_GetDefaultConfig(&config);
    config.enableAnalogPreliminary = true;
    config.referenceVoltageSource = kLPADC_ReferenceVoltageAlt2;
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

target_sense_status_t target_sense_convert(uint8_t channel, bool side_b, uint16_t* code) {
    if (!adc_ready) {
        target_sense_status_t const status = adc_init();
        if (status != TARGET_SENSE_OK) {
            return status;
        }
        adc_ready = true;
    }

    lpadc_conv_command_config_t command;
    LPADC_GetDefaultConvCommandConfig(&command);
    command.channelNumber = channel;
    command.sampleChannelMode = side_b ? kLPADC_SampleChannelSingleEndSideB : kLPADC_SampleChannelSingleEndSideA;
    command.conversionResolutionMode = kLPADC_ConversionResolutionHigh;
    command.hardwareAverageMode = kLPADC_HardwareAverageCount16;
    command.sampleTimeMode = kLPADC_SampleTimeADCK35;
    LPADC_SetConvCommandConfig(ADC0, SENSE_COMMAND_ID, &command);

    LPADC_DoSoftwareTrigger(ADC0, 1UL << SENSE_TRIGGER);
    lpadc_conv_result_t result;
    for (uint32_t i = 0U; i < SENSE_RESULT_POLLS; i++) {
        if (LPADC_GetConvResult(ADC0, &result, 0U)) {
            *code = result.convValue;
            return TARGET_SENSE_OK;
        }
    }
    return TARGET_SENSE_NO_RESULT;
}

bool target_sense_detected(void) {
    return GPIO->B[SENSE_DETECT_PORT][SENSE_DETECT_PIN] == 0U;
}
