/** Vendor 0x87: DPIDR reads with the SWDIO sample placed by the host. See sample_probe.c. */
#ifndef SAMPLE_PROBE_H
#define SAMPLE_PROBE_H

#include <stdint.h>

/// `at` flag for the lead shape: the low bits are then the high phase, in filler cycles.
#define SAMPLE_LEAD 0x80U

/// `at` value for production's placement: the lead shape with the whole fill as its high phase.
#define SAMPLE_BEFORE_EDGE 0xFFU

/// The longest filler run, in cycles, so the most the high phase can be stretched by.
#define SAMPLE_FILL_MAX 32U

uint32_t sample_probe_command(const uint8_t* request, uint8_t* response);

#endif
