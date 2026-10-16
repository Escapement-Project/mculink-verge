/*
 * Copyright (c) 2021 ARM Limited. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Follows CMSIS-DAP's MCU-LINK example, ser_num.c. Changed by the Escapement Project: the unique id
 * read directly, and a guard against an unprogrammed one.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include "ser_num.h"

/** The part's 128-bit UUID, in the NXP-programmed area of the protected flash region.
 *
 * UM11126 section 47.8: "It can be read from flash PFR region at register location 0x0009_FC70
 * onwards." So this needs no ROM API call, only a load.
 */
#define UUID_ADDRESS  0x0009FC70UL
#define UUID_BYTES    16U

/** Distinguishes our firmware from the stock image at a glance.
 *
 * The serial is what every host tool addresses a probe by, and the bench runs two MCU-Links side
 * by side with different firmware. A prefix means the one running this image is obvious in
 * `probe-rs list` without having to remember which unit was flashed.
 *
 * It does mean a probe's serial changes when this firmware is installed, so anything keyed on the
 * stock serial -- a udev rule, a saved probe selector -- has to be updated. NXP's encoding is not
 * reproducible from ARM's reference sources, which derive a different string entirely.
 */
#define SERIAL_PREFIX "CJ"

static char serial[16];

/** CRC-32 with polynomial 0x04C11DB7, as ARM's reference serial number code uses. */
static uint32_t crc32(const uint8_t *data, uint32_t len) {
  uint32_t crc = 0xFFFFFFFFU;

  while (len-- != 0U) {
    crc ^= ((uint32_t)*data++) << 24U;
    for (uint32_t bit = 8U; bit != 0U; bit--) {
      crc = (crc & 0x80000000U) ? ((crc << 1U) ^ 0x04C11DB7U) : (crc << 1U);
    }
  }
  return crc;
}

char *GetSerialNum(void) {
  if (serial[0] != '\0') {
    return serial;
  }

  const uint8_t *uuid = (const uint8_t *)UUID_ADDRESS;

  // An unprogrammed or unreadable region reads back uniform, and hashing that gives every unit the
  // same serial -- which is worse than having none, because two probes would then be
  // indistinguishable while looking fine. Say so in the string instead.
  uint8_t first = uuid[0];
  bool uniform = true;
  for (uint32_t i = 1U; i < UUID_BYTES; i++) {
    if (uuid[i] != first) {
      uniform = false;
      break;
    }
  }

  if (uniform) {
    snprintf(serial, sizeof(serial), "%sNOUUID", SERIAL_PREFIX);
  } else {
    snprintf(serial, sizeof(serial), "%s%08lX", SERIAL_PREFIX,
             (unsigned long)crc32(uuid, UUID_BYTES));
  }
  return serial;
}
