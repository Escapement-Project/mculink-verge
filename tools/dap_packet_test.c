#include <assert.h>
#include <stdlib.h>
#include <string.h>

#define FIRMWARE_VERSION "test"
#include "dap_packet.h"
#include "jtag_chain.h"

static dap_packet_plan_t check(const uint8_t* bytes, uint32_t length, uint32_t capacity, bool expected) {
    uint8_t* exact = malloc(length);
    assert(exact != NULL);
    memcpy(exact, bytes, length);
    dap_packet_plan_t plan = {0};
    assert(dap_packet_plan(exact, length, capacity, 4U, &plan) == expected);
    if (expected) {
        assert(plan.consumed > 0U && plan.consumed <= length);
        assert(plan.produced <= capacity);
    }
    free(exact);
    return plan;
}

static void prefixes(const uint8_t* bytes, uint32_t length, uint32_t capacity) {
    for (uint32_t i = 0U; i < length; i++) {
        check(bytes, i, capacity, false);
    }
    dap_packet_plan_t const plan = check(bytes, length, capacity, true);
    assert(plan.consumed == length);
}

int main(void) {
    assert(jtag_chain_valid(NULL, 0U, 8U));
    assert(jtag_chain_valid((uint8_t[]){1, 255, 4}, 3U, 8U));
    assert(!jtag_chain_valid((uint8_t[]){4, 4, 0}, 3U, 8U));
    assert(!jtag_chain_valid((uint8_t[]){0}, 1U, 8U));
    assert(!jtag_chain_valid(NULL, 9U, 8U));

    static const struct {
        uint8_t id, in, out;
    } fixed[] = {
        {0x00, 2, 4},   {0x01, 3, 2}, {0x02, 2, 2}, {0x03, 1, 2}, {0x04, 6, 2},  {0x08, 6, 2}, {0x09, 3, 2},
        {0x0a, 1, 3},   {0x10, 7, 2}, {0x11, 5, 2}, {0x13, 2, 2}, {0x15, 2, 2},  {0x16, 2, 6}, {0x17, 2, 2},
        {0x18, 2, 2},   {0x19, 5, 5}, {0x1a, 2, 2}, {0x1b, 1, 6}, {0x82, 1, 22}, {0x83, 1, 1}, {0x85, 1, 1}, {0x86, 3, 7},
        {0x87, 10, 17}, {0x8c, 2, 3}, {0x8e, 4, 11}, {0x8f, 2, 3}, {0x95, 2, 2}, {0x96, 3, 2},
    };
    for (uint32_t i = 0; i < sizeof(fixed) / sizeof(fixed[0]); i++) {
        uint8_t bytes[10] = {fixed[i].id};
        prefixes(bytes, fixed[i].in, 64U);
        assert(check(bytes, fixed[i].in, 64U, true).produced == fixed[i].out);
        check(bytes, fixed[i].in, fixed[i].out - 1U, false);
    }
#ifdef GENERATED_WIRE
    prefixes((uint8_t[]){0x8a, 0, 0, 0}, 4U, 64U);
    check((uint8_t[]){0x8a, 0, 0, 0}, 4U, 63U, false);
#else
    assert(check((uint8_t[]){0x8a}, 1U, 1U, true).produced == 1U);
#endif
    for (uint8_t mode = 8U; mode <= 13U; mode++) {
        uint8_t bytes[64] = {0x90U, mode};
        uint32_t const length = mode == 8U ? 6U : mode == 9U ? 7U : mode == 10U ? 3U : mode == 12U ? 2U : 26U;
        uint32_t const reply = mode == 8U || mode == 9U ? 31U : mode == 12U ? 32U : 3U;
        prefixes(bytes, length, reply);
        assert(check(bytes, length, reply, true).produced == reply);
        check(bytes, length, reply - 1U, false);
        dap_packet_plan_t const padded = check(bytes, sizeof(bytes), reply, true);
        assert(padded.consumed == length);

    }
    // Modes 6 and 15, Aux's alone, still carry a DIO mask a batch reads past.
    for (uint8_t mode = 6U; mode <= 15U; mode += 9U) {
        uint8_t bytes[64] = {0x90U, mode};
        prefixes(bytes, 3U, 2U);
        assert(check(bytes, sizeof(bytes), 2U, true).consumed == 3U);
    }
    {
        // Mode 14 carries its setting in the request, so a batch reads the next command after it.
        uint8_t bytes[64] = {0x90U, 14U, 1U};
        prefixes(bytes, 3U, 3U);
        assert(check(bytes, 3U, 3U, true).produced == 3U);
        check(bytes, 3U, 2U, false);
        assert(check(bytes, sizeof(bytes), 3U, true).consumed == 3U);
    }
    for (uint32_t capacity = 64U; capacity <= 512U; capacity += 448U) {
        prefixes((uint8_t[]){0x11, 0x80, 0x96, 0x98, 0}, 5U, capacity);
        prefixes((uint8_t[]){0x06, 0, 1, 0, 0, 0x80, 0xff, 0, 0x80}, 9U, capacity);
        prefixes((uint8_t[]){0x1d, 1, 8, 0x55}, 4U, capacity);
        prefixes((uint8_t[]){0x14, 1, 0x88, 0x55}, 4U, capacity);
        prefixes((uint8_t[]){0x12, 9, 0x55, 0x55}, 4U, capacity);
        prefixes((uint8_t[]){0x05, 0, 3, 2, 0x12, 0, 0, 0, 0, 0x20, 0, 0, 0, 0}, 14U, capacity);
        prefixes((uint8_t[]){0x90, 0, 0xe8, 3, 0, 0}, 6U, capacity);
        prefixes((uint8_t[]){0x91, 0, 0x80, 0x96, 0x98, 0, 1, 0, 0x80}, 9U, capacity);
        prefixes((uint8_t[]){0x92, 0, 0x80, 0x96, 0x98, 0, 1, 0, 1, 0, 0x80}, 11U, capacity);
        prefixes((uint8_t[]){0x94, 1, 0, 0, 0, 0}, 6U, capacity);
        prefixes((uint8_t[]){0x94, 1, 0, 0, 0, 2, 'h', 'i'}, 8U, capacity);
        assert(check((uint8_t[]){0x94, 1, 0, 0, 0, 2, 'h', 'i'}, 8U, capacity, true).produced == 3U);
        dap_packet_plan_t p = check((uint8_t[]){0x06, 0, 0xff, 0xff, 2}, 5U, capacity, true);
        assert(p.produced == capacity && p.limit == (capacity - 4U) / 4U);
        p = check((uint8_t[]){0x1c, 0xff, 0xff}, 3U, capacity, true);
        assert(p.produced == capacity && p.limit == capacity - 4U);
    }
    check((uint8_t[]){0x1d, 8, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80}, 10U, 64U, false);
    // A label the request does not hold, and a reply with no room.
    check((uint8_t[]){0x94, 1, 0, 0, 0, 3, 'h', 'i'}, 8U, 64U, false);
    check((uint8_t[]){0x94, 1, 0, 0, 0, 0}, 6U, 2U, false);
    check((uint8_t[]){0x05, 0, 1, 0x82}, 4U, 10U, false);
    check((uint8_t[]){0x05, 0, 1, 0x82}, 4U, 11U, true);
    for (uint32_t capacity = 0U; capacity < 4U; capacity++) {
        check((uint8_t[]){0x06, 0, 1, 0, 2}, 5U, capacity, false);
        check((uint8_t[]){0x1c, 0, 0}, 3U, capacity, false);
    }
    // Exact allocations catch reads hidden by USB buffers' unused backing storage.
    uint32_t seed = 7U;
    for (uint32_t length = 0U; length <= 512U; length++) {
        uint8_t* bytes = malloc(length);
        assert(bytes != NULL);
        for (uint32_t id = 0U; id < 256U; id++) {
            for (uint32_t i = 0U; i < length; i++) {
                seed = seed * 1664525U + 1013904223U;
                bytes[i] = (uint8_t)(seed >> 24U);
            }
            if (length != 0U) {
                bytes[0] = (uint8_t)id;
            }
            dap_packet_plan_t plan;
            if (dap_packet_plan(bytes, length, 64U, 4U, &plan)) {
                assert(plan.consumed > 0U && plan.consumed <= length && plan.produced <= 64U);
            }
        }
        free(bytes);
    }
}
