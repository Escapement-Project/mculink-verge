/** The MS OS 2.0 descriptor set that binds WinUSB to the vendor interfaces, built from the
 *  interfaces the configuration has. Free of TinyUSB and hardware, so tools/ms_os_20_test.c runs it
 *  on the host.
 *
 * Its nested length fields have to agree with the bytes after them, and nothing at runtime reports
 * a mismatch: Windows just binds no driver. So they are computed here, never written by hand.
 */
#ifndef MS_OS_20_H
#define MS_OS_20_H

#include <stddef.h>
#include <stdint.h>

/** Microsoft OS 2.0 Descriptors Specification, Table 9's descriptor types. */
#define MS_OS_20_TYPE_SET_HEADER 0x00U
#define MS_OS_20_TYPE_CONFIGURATION 0x01U
#define MS_OS_20_TYPE_FUNCTION 0x02U
#define MS_OS_20_TYPE_COMPATIBLE_ID 0x03U
#define MS_OS_20_TYPE_REGISTRY 0x04U

#define MS_OS_20_SET_HEADER_LEN 10U
#define MS_OS_20_CONFIGURATION_LEN 8U
/** A function subset header, the WINUSB compatible id, and DeviceInterfaceGUIDs with one GUID. */
#define MS_OS_20_FUNCTION_LEN (8U + 20U + 132U)
/** "{XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}" */
#define MS_OS_20_GUID_CHARS 38U

static inline uint8_t* ms_os_20_u16(uint8_t* out, uint16_t value) {
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8U);
    return out + 2;
}

/** ASCII to UTF-16LE, with the terminator when `terminated`. */
static inline uint8_t* ms_os_20_utf16(uint8_t* out, char const* text, size_t chars, int terminated) {
    for (size_t i = 0U; i < chars; i++) {
        out = ms_os_20_u16(out, (uint8_t)text[i]);
    }
    return terminated ? ms_os_20_u16(out, 0U) : out;
}

/** One function subset: WinUSB for the interface `first_interface`, under `guid`. */
static inline uint8_t* ms_os_20_function(uint8_t* out, uint8_t first_interface, char const* guid) {
    static char const name[] = "DeviceInterfaceGUIDs";
    out = ms_os_20_u16(out, 8U);
    out = ms_os_20_u16(out, MS_OS_20_TYPE_FUNCTION);
    *out++ = first_interface;
    *out++ = 0U;
    out = ms_os_20_u16(out, (uint16_t)MS_OS_20_FUNCTION_LEN);

    out = ms_os_20_u16(out, 20U);
    out = ms_os_20_u16(out, MS_OS_20_TYPE_COMPATIBLE_ID);
    static uint8_t const winusb[16] = {'W', 'I', 'N', 'U', 'S', 'B'};
    for (size_t i = 0U; i < sizeof(winusb); i++) {
        *out++ = winusb[i];
    }

    size_t const name_bytes = 2U * sizeof(name);
    // REG_MULTI_SZ: the GUID's own terminator, then the list's.
    size_t const data_bytes = 2U * (MS_OS_20_GUID_CHARS + 2U);
    out = ms_os_20_u16(out, (uint16_t)(10U + name_bytes + data_bytes));
    out = ms_os_20_u16(out, MS_OS_20_TYPE_REGISTRY);
    out = ms_os_20_u16(out, 7U);
    out = ms_os_20_u16(out, (uint16_t)name_bytes);
    out = ms_os_20_utf16(out, name, sizeof(name) - 1U, 1);
    out = ms_os_20_u16(out, (uint16_t)data_bytes);
    out = ms_os_20_utf16(out, guid, MS_OS_20_GUID_CHARS, 1);
    return ms_os_20_u16(out, 0U);
}

typedef struct {
    uint8_t first_interface;
    char const* guid;
} ms_os_20_binding_t;

/** The whole set for `count` bindings, into `out`, which holds the set's length: returned. */
static inline uint16_t ms_os_20_set(uint8_t* out, ms_os_20_binding_t const* bindings, size_t count) {
    uint16_t const total = (uint16_t)(MS_OS_20_SET_HEADER_LEN + MS_OS_20_CONFIGURATION_LEN + count * MS_OS_20_FUNCTION_LEN);
    uint8_t* at = ms_os_20_u16(out, MS_OS_20_SET_HEADER_LEN);
    at = ms_os_20_u16(at, MS_OS_20_TYPE_SET_HEADER);
    // Windows 8.1, the first to read the set.
    at = ms_os_20_u16(at, 0x0000U);
    at = ms_os_20_u16(at, 0x0603U);
    at = ms_os_20_u16(at, total);

    at = ms_os_20_u16(at, MS_OS_20_CONFIGURATION_LEN);
    at = ms_os_20_u16(at, MS_OS_20_TYPE_CONFIGURATION);
    *at++ = 0U;
    *at++ = 0U;
    at = ms_os_20_u16(at, (uint16_t)(total - MS_OS_20_SET_HEADER_LEN));
    for (size_t i = 0U; i < count; i++) {
        at = ms_os_20_function(at, bindings[i].first_interface, bindings[i].guid);
    }
    return total;
}

#endif
