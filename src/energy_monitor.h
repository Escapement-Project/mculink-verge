/** Background voltage/current capture on the MCU-Link Pro. */
#ifndef ENERGY_MONITOR_H
#define ENERGY_MONITOR_H

#include <stdbool.h>
#include <stdint.h>

#include "fsl_device_registers.h"

#include "energy_samples.h"

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
} energy_monitor_status_t;

typedef struct {
    uint16_t voltage_code;
    uint16_t current_code;
    uint16_t range_code;
    uint32_t sequence; /**< Complete pairs captured since the current start. */
    uint32_t voltage_mv;
    int32_t current_ua;
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

/** Start timer-triggered ADC capture at `rate_hz` paired samples per second. */
[[nodiscard]] energy_monitor_status_t energy_monitor_start(uint32_t rate_hz);
uint32_t energy_monitor_rate(void);

/** Stop acquisition and leave the ADC ready for on-demand target-sense reads. */
[[nodiscard]] energy_monitor_status_t energy_monitor_stop(void);

/** Return the newest complete pair of raw ADC codes, if one exists. */
[[nodiscard]] energy_monitor_status_t energy_monitor_latest(energy_monitor_sample_t* sample);

/** The low or high range's calibration points from the most recent start: count zero before any.
 *  ENERGY_MONITOR_ADC_ERROR unless that calibration succeeded. */
[[nodiscard]] energy_monitor_status_t energy_monitor_calibration(bool high_range, energy_calibration_t* calibration);

/** Read accumulated calibrated energy since the most recent successful start. */
[[nodiscard]] energy_monitor_status_t energy_monitor_accumulated(uint64_t* energy_nj, uint64_t* pairs);

/** Integrate one complete DMA segment. Called by CPU 0's DMA interrupt. */
void energy_monitor_process_segment(void);

/** Pause and resume capture around an on-demand ADC conversion. */
[[nodiscard]] bool energy_monitor_pause(void);
[[nodiscard]] bool energy_monitor_resume(void);

#endif
