/** Vendor 0x87: DPIDR reads with the SWDIO sample placed by the host. See sample_probe.c. */
#ifndef SAMPLE_PROBE_H
#define SAMPLE_PROBE_H

#include <stdint.h>

/// `at` value that samples before the rising edge, where every production read samples.
#define SAMPLE_BEFORE_EDGE 0xFFU

/// The longest filler run, in cycles, so the most the high phase can be stretched by.
#define SAMPLE_FILL_MAX 48U

uint32_t sample_probe_command(const uint8_t* request, uint8_t* response);

#endif
