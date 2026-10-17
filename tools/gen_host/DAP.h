/* Stand-in for ARM's DAP.h: the acknowledge codes and the DAP_Data fields the generator reads. */
#ifndef GEN_HOST_DAP_H
#define GEN_HOST_DAP_H

#include <stdint.h>

#define DAP_TRANSFER_OK (1U << 0)
#define DAP_TRANSFER_WAIT (1U << 1)
#define DAP_TRANSFER_FAULT (1U << 2)
#define DAP_TRANSFER_ERROR (1U << 3)

typedef struct {
    struct {
        uint8_t turnaround;
        uint8_t data_phase;
    } swd_conf;
    struct {
        uint8_t idle_cycles;
    } transfer;
    struct {
        uint8_t count;
        uint8_t index;
    } jtag_dev;
} DAP_Data_t;

extern DAP_Data_t DAP_Data;

#endif
