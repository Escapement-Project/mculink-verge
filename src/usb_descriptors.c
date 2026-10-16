/** USB descriptors for a CMSIS-DAP v2 probe.
 *
 * CMSIS-DAP v2 is a vendor-class interface with one bulk OUT and one bulk IN endpoint, which the
 * host finds through a WinUSB descriptor rather than by class. The interface string must begin
 * with "CMSIS-DAP": that prefix is how host tools, probe-rs included, tell a DAP interface from
 * any other vendor interface on the same device.
 *
 * The MS OS 2.0 descriptor below is generated rather than hand-written, because its three nested
 * length fields have to agree with the bytes that follow them and nothing at run time reports a
 * mismatch -- the device simply fails to bind.
 */
#include "tusb.h"
#include "ser_num.h"

#define USB_VID   0x1FC9      // NXP
#define USB_PID   0x0143      // MCU-Link CMSIS-DAP
#define USB_BCD   0x0210      // 2.1, which is what makes the host ask for the BOS descriptor

static const tusb_desc_device_t desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = USB_BCD,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

uint8_t const *tud_descriptor_device_cb(void) {
  return (uint8_t const *)&desc_device;
}

enum { ITF_NUM_VENDOR = 0, ITF_NUM_CDC, ITF_NUM_CDC_DATA, ITF_NUM_HID, ITF_NUM_TOTAL };

#define EPNUM_VENDOR_OUT  0x01
#define EPNUM_VENDOR_IN   0x81
#define EPNUM_CDC_NOTIF   0x82
#define EPNUM_CDC_OUT     0x03
#define EPNUM_CDC_IN      0x83
#define EPNUM_HID_OUT     0x04
#define EPNUM_HID_IN      0x84

#define CONFIG_TOTAL_LEN \
  (TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN + TUD_CDC_DESC_LEN + TUD_HID_INOUT_DESC_LEN)

/** A vendor-defined report pair, 64 bytes each way: what CMSIS-DAP v1 is. */
static uint8_t const desc_hid_report[] = {
    TUD_HID_REPORT_DESC_GENERIC_INOUT(CFG_TUD_HID_EP_BUFSIZE),
};

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) {
  (void)instance;
  return desc_hid_report;
}

static uint8_t const desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    TUD_VENDOR_DESCRIPTOR(ITF_NUM_VENDOR, 4, EPNUM_VENDOR_OUT, EPNUM_VENDOR_IN, 512),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 5, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 512),
    TUD_HID_INOUT_DESCRIPTOR(ITF_NUM_HID, 6, HID_ITF_PROTOCOL_NONE, sizeof(desc_hid_report),
                             EPNUM_HID_OUT, EPNUM_HID_IN, CFG_TUD_HID_EP_BUFSIZE, 1),
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
  (void)index;
  return desc_configuration;
}

// ---- Strings ------------------------------------------------------------------------------------

static char const *string_desc[] = {
    [0] = (const char[]){0x09, 0x04},   // English (US)
    [1] = "NXP Semiconductors",
    [2] = "MCU-Link CMSIS-DAP cJTAG",
    [3] = NULL,                           // serial, filled in from the part's unique id
    [4] = "CMSIS-DAP v2",                 // the prefix host tools match on
    [5] = "MCU-Link VCOM",
    [6] = "CMSIS-DAP v1",                 // the same prefix, on the HID interface
};

static uint16_t string_buffer[32];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
  (void)langid;
  size_t count;

  if (index == 0) {
    memcpy(&string_buffer[1], string_desc[0], 2);
    count = 1;
  } else {
    if (index >= TU_ARRAY_SIZE(string_desc)) {
      return NULL;
    }
    char const *text = (index == 3) ? GetSerialNum() : string_desc[index];
    count = strlen(text);
    if (count > TU_ARRAY_SIZE(string_buffer) - 1) {
      count = TU_ARRAY_SIZE(string_buffer) - 1;
    }
    for (size_t i = 0; i < count; i++) {
      string_buffer[1 + i] = text[i];
    }
  }

  string_buffer[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * count + 2));
  return string_buffer;
}

// ---- WinUSB, so the interface binds with no vendor driver ---------------------------------------

#define MS_OS_20_DESC_LEN         178u
#define VENDOR_REQUEST_MICROSOFT  2

#define BOS_TOTAL_LEN (TUD_BOS_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN)

static uint8_t const desc_bos[] = {
    TUD_BOS_DESCRIPTOR(BOS_TOTAL_LEN, 1),
    TUD_BOS_MS_OS_20_DESCRIPTOR(MS_OS_20_DESC_LEN, VENDOR_REQUEST_MICROSOFT),
};

uint8_t const *tud_descriptor_bos_cb(void) {
  return desc_bos;
}

uint8_t const desc_ms_os_20[] = {
    // Set header
    U16_TO_U8S_LE(0x000A), U16_TO_U8S_LE(MS_OS_20_SET_HEADER_DESCRIPTOR),
    U32_TO_U8S_LE(0x06030000), U16_TO_U8S_LE(MS_OS_20_DESC_LEN),

    // Configuration subset header
    U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_CONFIGURATION),
    0, 0, U16_TO_U8S_LE(MS_OS_20_DESC_LEN - 0x0A),

    // Function subset header
    U16_TO_U8S_LE(0x0008), U16_TO_U8S_LE(MS_OS_20_SUBSET_HEADER_FUNCTION),
    ITF_NUM_VENDOR, 0, U16_TO_U8S_LE(MS_OS_20_DESC_LEN - 0x0A - 0x08),

    // Compatible id: WINUSB
    U16_TO_U8S_LE(0x0014), U16_TO_U8S_LE(MS_OS_20_FEATURE_COMPATBLE_ID),
    'W', 'I', 'N', 'U', 'S', 'B', 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,

    // Registry property: DeviceInterfaceGUIDs = {CDB3B5AD-293B-4663-AA36-1AAE46463776}
    U16_TO_U8S_LE(0x0084), U16_TO_U8S_LE(MS_OS_20_FEATURE_REG_PROPERTY),
    U16_TO_U8S_LE(0x0007), U16_TO_U8S_LE(0x002a),
    0x44, 0x00, 0x65, 0x00, 0x76, 0x00, 0x69, 0x00, 0x63, 0x00,
    0x65, 0x00, 0x49, 0x00, 0x6E, 0x00, 0x74, 0x00, 0x65, 0x00,
    0x72, 0x00, 0x66, 0x00, 0x61, 0x00, 0x63, 0x00, 0x65, 0x00,
    0x47, 0x00, 0x55, 0x00, 0x49, 0x00, 0x44, 0x00, 0x73, 0x00,
    0x00, 0x00,
    U16_TO_U8S_LE(0x0050),
    0x7B, 0x00, 0x43, 0x00, 0x44, 0x00, 0x42, 0x00, 0x33, 0x00,
    0x42, 0x00, 0x35, 0x00, 0x41, 0x00, 0x44, 0x00, 0x2D, 0x00,
    0x32, 0x00, 0x39, 0x00, 0x33, 0x00, 0x42, 0x00, 0x2D, 0x00,
    0x34, 0x00, 0x36, 0x00, 0x36, 0x00, 0x33, 0x00, 0x2D, 0x00,
    0x41, 0x00, 0x41, 0x00, 0x33, 0x00, 0x36, 0x00, 0x2D, 0x00,
    0x31, 0x00, 0x41, 0x00, 0x41, 0x00, 0x45, 0x00, 0x34, 0x00,
    0x36, 0x00, 0x34, 0x00, 0x36, 0x00, 0x33, 0x00, 0x37, 0x00,
    0x37, 0x00, 0x36, 0x00, 0x7D, 0x00, 0x00, 0x00, 0x00, 0x00,
};

TU_VERIFY_STATIC(sizeof(desc_ms_os_20) == MS_OS_20_DESC_LEN, "MS OS 2.0 descriptor length mismatch");

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request) {
  if (stage != CONTROL_STAGE_SETUP) {
    return true;
  }
  if (request->bmRequestType_bit.type == TUSB_REQ_TYPE_VENDOR &&
      request->bRequest == VENDOR_REQUEST_MICROSOFT &&
      request->wIndex == 7) {
    uint16_t total_len;
    memcpy(&total_len, desc_ms_os_20 + 8, 2);
    return tud_control_xfer(rhport, request, (void *)(uintptr_t)desc_ms_os_20, total_len);
  }
  return false;
}
