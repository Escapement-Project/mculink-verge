/** Vendor 0x88: run host-supplied code from RAM and time it. See code_lab.c. */
#ifndef CODE_LAB_H
#define CODE_LAB_H

#include <stdint.h>

uint32_t code_lab_command(const uint8_t* request, uint8_t* response);

#endif
