#ifndef DAP_DISPATCH_H
#define DAP_DISPATCH_H

#include <stdbool.h>
#include <stdint.h>

/** Validate request framing and reply capacity before executing each command.
 *  response must hold packet_size bytes, at most DAP_PACKET_SIZE.
 */
uint32_t DAP_ExecuteCommandSized(const uint8_t* request, uint32_t request_length, uint32_t packet_size,
                                uint8_t* response);

/** Set while CPU 1 runs a Verge Aux request (src/core1_main.c): the commands only that interface
 *  has check it. */
extern bool dap_vendor_on_aux;

#endif
