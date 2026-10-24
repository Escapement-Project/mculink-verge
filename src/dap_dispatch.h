/** Buffer sizes the command layer needs, shared by main.c and the dispatcher. */
#ifndef DAP_DISPATCH_H
#define DAP_DISPATCH_H

#include "DAP_config.h"

/** The longest reply one command can produce.
 *
 * DAP_Transfer's count is a byte and each transfer can answer with four data bytes and four
 * timestamp bytes, with a posted read flushed at the end. Nothing in ARM's command layer checks a
 * reply against the packet size, so the buffer is what bounds it.
 */
#define DAP_COMMAND_RESPONSE_MAX (2U + (8U * 255U) + 4U)

/** DAP_ExecuteCommands stops once the reply has passed a packet, so one more command's worst case
 * is all the room needed beyond that. */
#define DAP_RESPONSE_SIZE (DAP_PACKET_SIZE + DAP_COMMAND_RESPONSE_MAX)

/** Validate request framing and reply capacity before executing each command. */
uint32_t DAP_ExecuteCommandSized(const uint8_t* request, uint32_t request_length, uint32_t packet_size,
                                uint8_t* response);

#endif
