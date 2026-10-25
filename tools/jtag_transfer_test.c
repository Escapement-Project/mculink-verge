#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define DAP_JTAG 1
#define DAP_JTAG_DEV_CNT 8
#define TIMESTAMP_CLOCK 150000000U
#include "DAP.h"

DAP_Data_t DAP_Data;
volatile uint8_t DAP_TransferAbort;
static unsigned resolved, transfers, ir_count, waits, abort_at;
static uint32_t word;
static uint8_t ack;

struct jtag_transfer_fns {
    void (*ir)(uint32_t);
    uint8_t (*transfer)(uint32_t, uint32_t*);
};

static void ir(uint32_t value) {
    assert(value == JTAG_DPACC || value == JTAG_APACC);
    ir_count++;
}

static uint8_t transfer(uint32_t request, uint32_t* data) {
    transfers++;
    if (transfers == abort_at) { DAP_TransferAbort = 1U; }
    if (waits != 0U) { waits--; return DAP_TRANSFER_WAIT; }
    if (ack == DAP_TRANSFER_OK && data != nullptr && (request & DAP_TRANSFER_RnW) != 0U) { *data = word; }
    DAP_Data.timestamp++;
    return ack;
}

static struct jtag_transfer_fns resolve_transfers(void) {
    assert(DAP_Data.jtag_dev.index < DAP_Data.jtag_dev.count);
    resolved++;
    return (struct jtag_transfer_fns){ir, transfer};
}

#include "jtag_transfer.h"

static void reset(void) {
    memset(&DAP_Data, 0, sizeof(DAP_Data));
    DAP_Data.jtag_dev.count = 2U;
    DAP_Data.transfer.retry_count = 2U;
    DAP_Data.transfer.match_retry = 2U;
    resolved = transfers = ir_count = waits = abort_at = 0U;
    word = 0x80000000U;
    ack = DAP_TRANSFER_OK;
}

static uint32_t run(const uint8_t* source, size_t length, uint8_t* response) {
    uint8_t* request = malloc(length);
    assert(request != nullptr);
    memcpy(request, source, length);
    uint32_t const result = JTAG_TransferCommand(request, response);
    assert((result >> 16U) == length);
    free(request);
    return result & 0xFFFFU;
}

int main(void) {
    uint8_t response[256];
    const uint8_t reads[] = {1U, 3U, 0x0FU, 0x0FU, 0x0FU};
    reset();
    assert(run(reads, sizeof(reads), response) == 14U);
    assert(resolved == 1U && transfers == 4U && ir_count == 2U && response[0] == 3U && response[1] == 1U);
    assert(read_u32_le(response + 2U) == word && read_u32_le(response + 10U) == word);
    const uint8_t writes[] = {0U, 2U, 0U, 0U, 0U, 0U, 0x80U, 1U, 0xFFU, 0xFFU, 0xFFU, 0xFFU};
    reset();
    assert(run(writes, sizeof(writes), response) == 2U);
    assert(resolved == 1U && transfers == 3U && ir_count == 3U && response[0] == 2U);
    const uint8_t match[] = {0U, 2U, 0x20U, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0x1FU, 0U, 0U, 0U, 0x80U};
    reset();
    assert(run(match, sizeof(match), response) == 2U && response[0] == 2U && response[1] == 1U);
    assert(DAP_Data.transfer.match_mask == UINT32_MAX && resolved == 1U);
    reset(); word = 0U;
    assert(run(match, sizeof(match), response) == 2U && (response[1] & DAP_TRANSFER_MISMATCH) != 0U);
    const uint8_t stamped[] = {0U, 2U, 0x8FU, 0x82U};
    reset();
    assert(run(stamped, sizeof(stamped), response) == 18U && response[0] == 2U);
    assert(resolved == 1U && ir_count == 2U);
    reset(); waits = 1U;
    assert(run(reads, sizeof(reads), response) == 14U && transfers == 5U && response[1] == 1U);
    reset(); waits = 4U;
    assert(run(reads, sizeof(reads), response) == 2U && transfers == 3U && response[1] == DAP_TRANSFER_WAIT);
    reset(); ack = DAP_TRANSFER_FAULT;
    assert(run(writes, sizeof(writes), response) == 2U && transfers == 1U && response[0] == 0U);
    reset(); abort_at = 1U;
    assert(run(reads, sizeof(reads), response) == 6U && response[0] == 1U && resolved == 1U);
    reset();
    uint8_t invalid[] = {2U, 0U};
    assert(JTAG_TransferCommand(invalid, response) == ((1U << 16U) | 2U) && resolved == 0U && transfers == 0U);
    uint8_t empty[] = {0U, 0U};
    assert(run(empty, sizeof(empty), response) == 2U && resolved == 1U && transfers == 0U);
}
