/** Background voltage/current capture on the MCU-Link Pro. */
#ifndef ENERGY_MONITOR_H
#define ENERGY_MONITOR_H

#include <stdbool.h>
#include <stdint.h>

#include "fsl_device_registers.h"

#include "energy_samples.h"
#include "energy_identity.h"

#define ENERGY_DMA_CHANNEL 21U
#define ENERGY_DMA_MASK (1UL << ENERGY_DMA_CHANNEL)
#define ENERGY_MAX_RATE_HZ 100000U

typedef enum {
    ENERGY_MONITOR_OK = 0,
    ENERGY_MONITOR_STOPPED,
    ENERGY_MONITOR_NO_SAMPLE,
    ENERGY_MONITOR_UNAVAILABLE,
    ENERGY_MONITOR_BAD_RATE,
    ENERGY_MONITOR_BAD_ARGUMENT,
    ENERGY_MONITOR_ADC_ERROR,
    ENERGY_MONITOR_DMA_ERROR,
    ENERGY_MONITOR_STALE_CAPTURE,
    ENERGY_MONITOR_IDENTITY_ERROR,
} energy_monitor_status_t;

typedef struct {
    uint16_t voltage_code;
    uint16_t current_code;
    uint16_t range_code;
    uint32_t sequence; /**< Complete pairs captured since the current start. */
    uint32_t voltage_mv;
    int32_t current_na;
    uint8_t range_id;
    bool calibrated;
} energy_monitor_sample_t;

void energy_monitor_init(void);

/** A rising edge on CTRL_HIGH_CURR_EN (PIO0_17). It sets U18, the low range, only while the
 *  comparator holds U18's reset high. The barrier keeps the low time to two bus cycles at least,
 *  against the '1G175's 3.5 ns. */
static inline void energy_latch_clock(void) {
    GPIO->CLR[0] = 1UL << 17U;
    __DSB();
    GPIO->SET[0] = 1UL << 17U;
    __DSB();
}

/** Calibrate the current ranges for the jumper setting, before CPU 1 serves commands. Calibration
 *  opens J9 for its length, about 0.2 s; a start calibrates again only when nothing is calibrated
 *  or the setting has changed. Waits for CPU 0 to reach its loop. */
void energy_monitor_calibrate(void);

/** What a capture may measure the current on (`0x90` mode 14, docs/host-interface.md). Automatic is
 *  the default. A low range alone is not offered: holding it would take the comparator's switch up,
 *  the only protection a target drawing past its full scale has, off U18's reset. */
#define ENERGY_RANGES_AUTO 0U
#define ENERGY_RANGES_HIGH 1U

/** Sets the ranges the next capture's start uses; after it, automatic again. A running capture
 *  keeps its own. */
[[nodiscard]] energy_monitor_status_t energy_monitor_set_ranges(uint8_t ranges);

/** Start timer-triggered ADC capture at `rate_hz` paired samples per second. */
[[nodiscard]] energy_monitor_status_t energy_monitor_start(uint32_t rate_hz);
/** The same, with Verge Aux's stream beginning at its first pair (src/energy_stream.h) and reporting
 *  the edges of the DIO pins in `dio_mask` it can. */
[[nodiscard]] energy_monitor_status_t energy_monitor_start_stream(uint32_t rate_hz, uint8_t dio_mask);
[[nodiscard]] energy_monitor_status_t energy_monitor_start_identified(uint32_t rate_hz, bool stream, uint8_t dio_mask);
[[nodiscard]] energy_monitor_status_t energy_monitor_identity(energy_capture_id_t* id, uint32_t* rate);
[[nodiscard]] energy_monitor_status_t energy_monitor_stop_identified(uint8_t const* id);
uint32_t energy_monitor_rate(void);

/** Stop acquisition and leave the ADC ready for on-demand target-sense reads. */
[[nodiscard]] energy_monitor_status_t energy_monitor_stop(void);

/** Return the newest complete pair of raw ADC codes, if one exists. */
[[nodiscard]] energy_monitor_status_t energy_monitor_latest(energy_monitor_sample_t* sample);

/** The low or high range's calibration points from the most recent calibration: count zero before
 *  any. ENERGY_MONITOR_ADC_ERROR unless that calibration succeeded. */
[[nodiscard]] energy_monitor_status_t energy_monitor_calibration(bool high_range, energy_calibration_t* calibration);

/** Read accumulated calibrated energy since the most recent successful start. */
[[nodiscard]] energy_monitor_status_t energy_monitor_accumulated(uint64_t* energy_nj, uint64_t* pairs);

/** Records a marker in Verge Aux's energy stream, stamped now
 *  (docs/host-interface.md, "Markers: 0x94"): ENERGY_STREAM_MARK_*, or false through `available`
 *  without energy capture. */
[[nodiscard]] uint8_t energy_monitor_mark(uint32_t number, uint8_t const* label, uint8_t length, bool* available);

/** Pause and resume capture around an on-demand ADC conversion. */
[[nodiscard]] bool energy_monitor_pause(void);
[[nodiscard]] bool energy_monitor_resume(void);

/** Stop the conversion trigger for a latency measurement, so that DMA0 stops reading ADC0 on the
 *  AHB port the bit paths use (BUSES.md). Returns whether it held a running capture; give that to
 *  energy_monitor_release. Nothing is converted meanwhile, so the energy total leaves the held time
 *  out. */
[[nodiscard]] bool energy_monitor_hold(void);
void energy_monitor_release(bool held);

#endif
