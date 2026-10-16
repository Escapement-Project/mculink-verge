/*
 * Copyright (c) 2021 ARM Limited. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Follows CMSIS-DAP's MCU-LINK example, ser_num.c. Changed by the Escapement Project: the unique id
 * read directly, and a guard against an unprogrammed one.
 */
#include <stdio.h>
#include "bsp/board_api.h"
#include "ser_num.h"

static char serial[25];

char *GetSerialNum(void) {
  if (serial[0] == '\0') {
    uint8_t id[16];
    size_t len = board_get_unique_id(id, sizeof(id));
    char *out = serial;
    for (size_t i = 0; i < len && out + 2 < serial + sizeof(serial); i++) {
      out += sprintf(out, "%02X", id[i]);
    }
    *out = '\0';
  }
  return serial;
}
