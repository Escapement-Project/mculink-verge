#ifndef DAP_DISPATCH_H
#define DAP_DISPATCH_H

#include <stdint.h>

/** Validate request framing and reply capacity before executing each command.
 *  response must hold packet_size bytes, at most DAP_PACKET_SIZE.
 */
uint32_t DAP_ExecuteCommandSized(const uint8_t* request, uint32_t request_length, uint32_t packet_size,
                                uint8_t* response);

#endif
