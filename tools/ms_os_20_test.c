#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ms_os_20.h"

static char const guid_cmsis_dap[] = "{CDB3B5AD-293B-4663-AA36-1AAE46463776}";
static char const guid_aux[] = "{A297EA18-39D8-41BE-B2E8-884CEC8FE819}";

/** The set src/usb_descriptors.c carried as literal bytes before it was built: CMSIS-DAP alone. */
static uint8_t const cmsis_dap_only[178] = {
    0x0A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x06, 0xB2, 0x00,
    0x08, 0x00, 0x01, 0x00, 0x00, 0x00, 0xA8, 0x00,
    0x08, 0x00, 0x02, 0x00, 0x00, 0x00, 0xA0, 0x00,
    0x14, 0x00, 0x03, 0x00, 'W', 'I', 'N', 'U', 'S', 'B', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0x84, 0x00, 0x04, 0x00, 0x07, 0x00, 0x2A, 0x00,
    0x44, 0x00, 0x65, 0x00, 0x76, 0x00, 0x69, 0x00, 0x63, 0x00,
    0x65, 0x00, 0x49, 0x00, 0x6E, 0x00, 0x74, 0x00, 0x65, 0x00,
    0x72, 0x00, 0x66, 0x00, 0x61, 0x00, 0x63, 0x00, 0x65, 0x00,
    0x47, 0x00, 0x55, 0x00, 0x49, 0x00, 0x44, 0x00, 0x73, 0x00,
    0x00, 0x00,
    0x50, 0x00,
    0x7B, 0x00, 0x43, 0x00, 0x44, 0x00, 0x42, 0x00, 0x33, 0x00,
    0x42, 0x00, 0x35, 0x00, 0x41, 0x00, 0x44, 0x00, 0x2D, 0x00,
    0x32, 0x00, 0x39, 0x00, 0x33, 0x00, 0x42, 0x00, 0x2D, 0x00,
    0x34, 0x00, 0x36, 0x00, 0x36, 0x00, 0x33, 0x00, 0x2D, 0x00,
    0x41, 0x00, 0x41, 0x00, 0x33, 0x00, 0x36, 0x00, 0x2D, 0x00,
    0x31, 0x00, 0x41, 0x00, 0x41, 0x00, 0x45, 0x00, 0x34, 0x00,
    0x36, 0x00, 0x34, 0x00, 0x36, 0x00, 0x33, 0x00, 0x37, 0x00,
    0x37, 0x00, 0x36, 0x00, 0x7D, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static uint16_t u16(uint8_t const* at) {
    return (uint16_t)(at[0] | (at[1] << 8U));
}

/** UTF-16LE at `at` spells `text` and ends in a terminator. */
static void expect_utf16(uint8_t const* at, char const* text) {
    size_t const n = strlen(text);
    for (size_t i = 0U; i < n; i++) {
        assert(u16(at + 2U * i) == (uint8_t)text[i]);
    }
    assert(u16(at + 2U * n) == 0U);
}

/** Builds the set into a buffer exactly its size, so a write past the end trips the sanitizer, and
 *  walks every length field against where the bytes actually end. */
static uint8_t* build(ms_os_20_binding_t const* bindings, size_t count, uint16_t* total) {
    size_t const size = MS_OS_20_SET_HEADER_LEN + MS_OS_20_CONFIGURATION_LEN + count * MS_OS_20_FUNCTION_LEN;
    uint8_t* const set = malloc(size);
    assert(set != NULL);
    *total = ms_os_20_set(set, bindings, count);
    assert(*total == size);

    assert(u16(set) == MS_OS_20_SET_HEADER_LEN && u16(set + 2U) == MS_OS_20_TYPE_SET_HEADER);
    assert(u16(set + 8U) == size);
    uint8_t const* configuration = set + MS_OS_20_SET_HEADER_LEN;
    assert(u16(configuration) == MS_OS_20_CONFIGURATION_LEN && u16(configuration + 2U) == MS_OS_20_TYPE_CONFIGURATION);
    assert(u16(configuration + 6U) == size - MS_OS_20_SET_HEADER_LEN);

    uint8_t const* at = configuration + MS_OS_20_CONFIGURATION_LEN;
    for (size_t i = 0U; i < count; i++) {
        uint8_t const* const function = at;
        assert(u16(at) == 8U && u16(at + 2U) == MS_OS_20_TYPE_FUNCTION);
        assert(at[4] == bindings[i].first_interface && at[5] == 0U);
        uint16_t const subset = u16(at + 6U);
        at += 8U;
        assert(u16(at) == 20U && u16(at + 2U) == MS_OS_20_TYPE_COMPATIBLE_ID);
        assert(memcmp(at + 4U, "WINUSB\0\0\0\0\0\0\0\0\0\0", 16U) == 0);
        at += 20U;
        uint16_t const property = u16(at);
        assert(u16(at + 2U) == MS_OS_20_TYPE_REGISTRY && u16(at + 4U) == 7U);
        uint16_t const name = u16(at + 6U);
        expect_utf16(at + 8U, "DeviceInterfaceGUIDs");
        assert(name == 2U * (strlen("DeviceInterfaceGUIDs") + 1U));
        uint16_t const data = u16(at + 8U + name);
        expect_utf16(at + 10U + name, bindings[i].guid);
        // REG_MULTI_SZ ends in an empty string.
        assert(u16(at + 10U + name + data - 2U) == 0U);
        assert(property == 10U + name + data);
        at += property;
        assert((size_t)(at - function) == subset);
    }
    assert((size_t)(at - set) == size);
    return set;
}

int main(void) {
    uint16_t total = 0U;

    ms_os_20_binding_t const debug_only[] = {{0U, guid_cmsis_dap}};
    uint8_t* set = build(debug_only, 1U, &total);
    assert(total == sizeof(cmsis_dap_only) && memcmp(set, cmsis_dap_only, total) == 0);
    free(set);

    // The Pro with every function: Verge Aux is interface 6, after both consoles and v1.
    ms_os_20_binding_t const pro[] = {{0U, guid_cmsis_dap}, {6U, guid_aux}};
    set = build(pro, 2U, &total);
    free(set);

    // The Pro with its SWD jumper fitted: Verge Aux alone.
    ms_os_20_binding_t const aux_only[] = {{4U, guid_aux}};
    set = build(aux_only, 1U, &total);
    free(set);

    set = build(NULL, 0U, &total);
    free(set);
    return 0;
}
