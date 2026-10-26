/** What the probe can see of the target's supply without talking to it.
 *
 * J6 pin 1 is a sense input, not a supply. A voltage follower buffers it, the buffered level is
 * the reference for every level shifter on the debug lines, and a 2K/2K divider brings half of it
 * to an ADC pin. With nothing connected, 33K to the probe's own 3V3 holds pin 1 up, so an empty
 * connector reads like a 3.3 V target; J6 pin 9, grounded by a standard target header, is what
 * tells the two apart. A target that is connected but unpowered pulls pin 1 well below a live
 * supply but not to zero: how far depends on how hard the dead board loads the 33K.
 *
 * Nothing here runs unless a host asks, apart from a Pro's energy calibration at boot. Every wait
 * on the converter is bounded: a sense read that cannot finish reports an error rather than hanging
 * the probe.
 */
#ifndef TARGET_SENSE_H
#define TARGET_SENSE_H

#include <stdbool.h>
#include <stdint.h>

/** Put the two sense pins in their modes: the divider's pin analog, the detect pin pulled up.
 *  At boot, so the pull-up has long settled by the time anything reads it. The converter itself
 *  stays off until a host asks for a reading, or a Pro with energy capture calibrates at boot. */
void target_sense_init(void);

/** Why a reading failed. Vendor 0x86 reports it in an error reply's code field, where 0 is a
 *  mode the probe does not know. */
typedef enum {
    TARGET_SENSE_OK = 0,
    TARGET_SENSE_OFFSET_CALIBRATION = 1,
    TARGET_SENSE_GAIN_CALIBRATION = 2,
    TARGET_SENSE_NO_RESULT = 3,
} target_sense_status_t;

typedef enum {
    TARGET_SENSE_CURRENT_RANGE_UNKNOWN = 0,
    TARGET_SENSE_CURRENT_RANGE_350_MA,
    TARGET_SENSE_CURRENT_RANGE_50_MA,
} target_sense_current_range_t;

/** Convert one ADC0 channel, side A or B, to a 16-bit code against VREFP. A converter that
 *  failed to come up is brought up again on the next call. */
target_sense_status_t target_sense_convert(uint8_t channel, bool side_b, uint16_t* code);

/** VREFP in millivolts, which code 65536 stands for. */
uint32_t target_sense_full_scale_mv(void);

/** Read voltage and current through the monitor's chained conversion path, averaged 128 times. */
target_sense_status_t target_sense_convert_pair_for_calibration(uint16_t* voltage_code, uint16_t* current_code);

/** Initialize ADC0, then configure chained voltage/current conversions for the monitor. */
target_sense_status_t target_sense_adc_init(void);
target_sense_current_range_t target_sense_current_range(void);
void target_sense_configure_monitor(uint32_t rate_hz);

/** True while something grounds J6 pin 9, which a standard target header does. */
bool target_sense_detected(void);

#endif
