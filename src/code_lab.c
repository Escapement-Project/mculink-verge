/** Vendor 0x88: run host-supplied code from RAM and time it.
 *
 * A bench instrument for the emulator's cost model, built only with the CODE_LAB option. The host
 * assembles a Thumb function, writes it into an arena, and calls it with interrupts masked. The
 * function reads DWT->CYCCNT itself around the code under test, so nothing of the call reaches
 * the measurement, and returns what it measured in r0 and r1.
 *
 * Two arenas, because where code runs from is one of the things being measured:
 *
 * - **SRAM**, a buffer in .bss at 0x2000xxxx. The core fetches it over S-AHB, the same port the
 *   GPIO stores go out on, which is where the bit paths in .ramfunc run today.
 * - **SRAMX**, the 32 KB at 0x04000000 that this image leaves unused. The core fetches it over
 *   C-AHB, so instruction fetch and GPIO traffic do not share a port.
 *
 * Request, after the id:
 *
 *     [0]  op     LAB_INFO, LAB_WRITE, LAB_READ or LAB_RUN
 *
 *     LAB_INFO   reply: status, arena count, then base and size of each, u32 little-endian
 *     LAB_WRITE  [1] arena, [2..3] offset, [4..5] length, [6..] bytes; reply: status
 *     LAB_READ   [1] arena, [2..3] offset, [4..5] length; reply: status, bytes
 *     LAB_RUN    [1] arena, [2..3] entry offset, [4..19] r0..r3; reply: status, r0, r1
 *
 * Every offset and length is checked against the arena and the packet. A function that does not
 * return hangs the probe with interrupts off; that is the host's to avoid.
 */
#include <stdbool.h>
#include <string.h>

#include "DAP_config.h"

#include "DAP.h"
#include "code_lab.h"

#define LAB_INFO 0U
#define LAB_WRITE 1U
#define LAB_READ 2U
#define LAB_RUN 3U

#define LAB_SRAM_SIZE 4096U
#define LAB_SRAMX_BASE 0x04000000U
#define LAB_SRAMX_SIZE 0x8000U

#define LAB_WRITE_HEADER 7U // id, op, arena, offset, length
#define LAB_READ_HEADER 2U  // id, status
#define LAB_RUN_REQUEST 21U

static uint8_t lab_sram[LAB_SRAM_SIZE] __attribute__((aligned(32)));

struct arena {
    uint8_t* base;
    uint32_t size;
};

static bool arena_get(uint8_t index, struct arena* out) {
    if (index == 0U) {
        *out = (struct arena){lab_sram, LAB_SRAM_SIZE};
        return true;
    }
    if (index == 1U) {
        *out = (struct arena){(uint8_t*)LAB_SRAMX_BASE, LAB_SRAMX_SIZE};
        return true;
    }
    return false;
}

static uint32_t get16(const uint8_t* in) { return (uint32_t)in[0] | ((uint32_t)in[1] << 8); }

static uint32_t get32(const uint8_t* in) { return get16(in) | (get16(in + 2) << 16); }

static void put32(uint8_t* out, uint32_t v) {
    out[0] = (uint8_t)v;
    out[1] = (uint8_t)(v >> 8);
    out[2] = (uint8_t)(v >> 16);
    out[3] = (uint8_t)(v >> 24);
}

typedef uint64_t (*lab_fn)(uint32_t, uint32_t, uint32_t, uint32_t);

uint32_t code_lab_command(const uint8_t* request, uint8_t* response) {
    const uint32_t op = request[1];
    struct arena a;
    response[1] = DAP_ERROR;

    if (op == LAB_INFO) {
        response[1] = DAP_OK;
        response[2] = 2U;
        uint8_t* out = response + 3;
        for (uint8_t i = 0U; i < 2U; i++) {
            (void)arena_get(i, &a);
            put32(out, (uint32_t)a.base);
            put32(out + 4, a.size);
            out += 8;
        }
        return (2U << 16) | (uint32_t)(out - response);
    }

    if (op == LAB_WRITE) {
        const uint32_t offset = get16(request + 3);
        const uint32_t length = get16(request + 5);
        if (length > DAP_PACKET_SIZE - LAB_WRITE_HEADER || !arena_get(request[2], &a) || offset > a.size ||
            length > a.size - offset) {
            return (LAB_WRITE_HEADER << 16) | 2U;
        }
        memcpy(a.base + offset, request + LAB_WRITE_HEADER, length);
        response[1] = DAP_OK;
        return ((LAB_WRITE_HEADER + length) << 16) | 2U;
    }

    if (op == LAB_READ) {
        const uint32_t offset = get16(request + 3);
        const uint32_t length = get16(request + 5);
        if (length > DAP_PACKET_SIZE - LAB_READ_HEADER || !arena_get(request[2], &a) || offset > a.size ||
            length > a.size - offset) {
            return (7U << 16) | 2U;
        }
        memcpy(response + LAB_READ_HEADER, a.base + offset, length);
        response[1] = DAP_OK;
        return (7U << 16) | (LAB_READ_HEADER + length);
    }

    if (op == LAB_RUN) {
        const uint32_t entry = get16(request + 3);
        if (!arena_get(request[2], &a) || entry >= a.size || (entry & 1U) != 0U) {
            return (LAB_RUN_REQUEST << 16) | 2U;
        }
        const lab_fn fn = (lab_fn)((uint32_t)(a.base + entry) | 1U);
        const uint32_t primask = __get_PRIMASK();
        __disable_irq();
        // The arena was written as data; the barriers make the core fetch what is there now.
        __DSB();
        __ISB();
        const uint64_t result = fn(get32(request + 5), get32(request + 9), get32(request + 13), get32(request + 17));
        __set_PRIMASK(primask);
        response[1] = DAP_OK;
        put32(response + 2, (uint32_t)result);
        put32(response + 6, (uint32_t)(result >> 32));
        return (LAB_RUN_REQUEST << 16) | 10U;
    }

    return (2U << 16) | 2U;
}
