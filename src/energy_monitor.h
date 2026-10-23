/** Background voltage/current capture on the MCU-Link Pro. */
#ifndef ENERGY_MONITOR_H
#define ENERGY_MONITOR_H

#include <stdbool.h>
#include <stdint.h>

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
    uint32_t sequence;
} energy_monitor_sample_t;

/** Start timer-triggered ADC capture at `rate_hz` paired samples per second. */
energy_monitor_status_t energy_monitor_start(uint32_t rate_hz);
uint32_t energy_monitor_rate(void);

/** Stop acquisition and leave the ADC ready for on-demand target-sense reads. */
void energy_monitor_stop(void);

/** Return the newest complete pair of raw ADC codes, if one exists. */
energy_monitor_status_t energy_monitor_latest(energy_monitor_sample_t* sample);

/** Pause and resume capture around an on-demand ADC conversion. */
void energy_monitor_pause(void);
void energy_monitor_resume(void);

#endif
