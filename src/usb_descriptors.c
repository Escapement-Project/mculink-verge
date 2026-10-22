/** USB descriptors for a CMSIS-DAP v2 probe.
 *
 * CMSIS-DAP v2 is a vendor-class interface with one bulk OUT and one bulk IN endpoint, which the
 * host finds through a WinUSB descriptor rather than by class. The interface string must begin
 * with "CMSIS-DAP": that prefix is how host tools, probe-rs included, tell a DAP interface from
 * any other vendor interface on the same device.
 *
 * The MS OS 2.0 descriptor below is generated rather than hand-written, because its three nested
 * length fields have to agree with the bytes that follow them and nothing at run time reports a
 * mismatch -- the device simply fails to bind. Its request handling follows TinyUSB's
 * webusb_serial example (MIT, THIRD-PARTY-NOTICES.md).
 */
// DAP_config.h first: DAP.h reads the DAP_SWD and DAP_JTAG settings it defines, so a sorted
// include block that puts DAP.h ahead of it will not compile. The names the probe answers to
// live there, so the USB strings and DAP_Info cannot drift apart.
#include "DAP_config.h"

#include "board_straps.h"
#include "dap_core.h"
#include "dap_shared.h"
#include "usb_descriptors.h"
#include "usb_transport.h"
#include "ser_num.h"
#include "reboot.h"
#include "reboot_usb.h"
#include "tusb.h"

#define USB_VID 0x1FC9 // NXP
#define USB_PID 0x0143 // MCU-Link CMSIS-DAP

/** 2.1 makes the host ask for the BOS, which carries the MS OS 2.0 descriptor that binds WinUSB to
 *  the CMSIS-DAP interface. Without that interface there is no BOS, and a device of 2.1 or later
 *  must have one, so it is 2.0. */
#define USB_BCD_BOS 0x0210
#define USB_BCD_NO_BOS 0x0200

static tusb_desc_device_t desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_VID,
    .idProduct = USB_PID,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

/** The functions this board and its jumpers leave (board_straps.h), as bits: what the
 *  configuration holds, and the low byte of bcdDevice. */
#define FUNCTION_DEBUG (1U << 0)
#define FUNCTION_VCOM (1U << 1)
#define FUNCTION_VCOM2 (1U << 2)

static uint32_t functions(void) {
    uint8_t const board = dap_shared.board;
    uint8_t const jumpers = dap_shared.jumpers;
    return (board_has_debug(board, jumpers) ? FUNCTION_DEBUG : 0U) |
           (board_has_vcom(board, jumpers) ? FUNCTION_VCOM : 0U) |
           (board_has_vcom2(board, jumpers) ? FUNCTION_VCOM2 : 0U);
}

/** Windows keeps what it learnt about a device by VID, PID and bcdDevice, so each set of
 *  functions gets a bcdDevice of its own, as stock's does for each jumper state. */
uint8_t const* tud_descriptor_device_cb(void) {
    uint32_t const present = functions();
    desc_device.bcdUSB = ((present & FUNCTION_DEBUG) != 0U) ? USB_BCD_BOS : USB_BCD_NO_BOS;
    desc_device.bcdDevice = (uint16_t)(0x0100U | present);
    return (uint8_t const*)&desc_device;
}

/** One endpoint layout for both boards: the plain MCU-Link is a Pro with
 *  functions removed. A function a board lacks, or a jumper turns off, is left out of the
 *  configuration, never declared as an interface that does nothing, as stock leaves it out. Its
 *  endpoint numbers stay reserved for it, and the interfaces after it renumber. CMSIS-DAP v2 is
 *  interface 0 on EP1 whenever it is there: the MS OS 2.0 descriptor names interface 0. */
#define ITF_NUM_VENDOR 0
#define EPNUM_VENDOR_OUT 0x01
#define EPNUM_VENDOR_IN 0x81
// EP2 is the Pro's LPCSIO HID, once USB-SIO exists.
#define EPNUM_VCOM_OUT 0x03
#define EPNUM_VCOM_IN 0x83
#define EPNUM_HID_OUT 0x04
#define EPNUM_HID_IN 0x84
#define EPNUM_VCOM2_OUT 0x05
#define EPNUM_VCOM2_IN 0x85
/** The notification endpoints are numbers USB1 does not have (it has EP0 to EP5), declared as
 *  NXP's stock firmware declares them on the Pro and never opened: this firmware sends no
 *  notification, and the boards wire no modem lines. */
#define EPNUM_VCOM_NOTIF 0x86
#define EPNUM_VCOM2_NOTIF 0x87

TU_VERIFY_STATIC(CFG_TUD_EDPT_PLACEHOLDER, "the notification endpoints are placeholders");

void usb_descriptors_init(void) {
    tud_configure_edpt_placeholder_t const placeholders = {
        .bm_in = TU_BIT(tu_edpt_number(EPNUM_VCOM_NOTIF)) | TU_BIT(tu_edpt_number(EPNUM_VCOM2_NOTIF)),
        .bm_out = 0U,
    };
    (void)tud_configure(BOARD_TUD_RHPORT, TUD_CFGID_EDPT_PLACEHOLDER, &placeholders);
}

#define STRID_VENDOR_ITF 4
#define STRID_VCOM 5
#define STRID_HID 6
#define STRID_VCOM2 7

/** TUD_CDC_DESCRIPTOR, with the notification endpoint as stock declares it: room for a whole
 *  SERIAL_STATE notification, polled every 2^15 microframes rather than every one. */
#define VCOM_NOTIF_SIZE 16
#define VCOM_NOTIF_INTERVAL 16
// clang-format off: one descriptor a line, as TinyUSB's own template lays it out.
#define VCOM_DESCRIPTOR(_itfnum, _stridx, _ep_notif, _epout, _epin) \
    /* Interface Associate */ \
    8, TUSB_DESC_INTERFACE_ASSOCIATION, _itfnum, 2, TUSB_CLASS_CDC, CDC_COMM_SUBCLASS_ABSTRACT_CONTROL_MODEL, CDC_COMM_PROTOCOL_NONE, 0, \
    /* CDC Control Interface */ \
    9, TUSB_DESC_INTERFACE, _itfnum, 0, 1, TUSB_CLASS_CDC, CDC_COMM_SUBCLASS_ABSTRACT_CONTROL_MODEL, CDC_COMM_PROTOCOL_NONE, _stridx, \
    /* CDC Header */ \
    5, TUSB_DESC_CS_INTERFACE, CDC_FUNC_DESC_HEADER, U16_TO_U8S_LE(0x0120), \
    /* CDC Call */ \
    5, TUSB_DESC_CS_INTERFACE, CDC_FUNC_DESC_CALL_MANAGEMENT, 0, (uint8_t)((_itfnum) + 1), \
    /* CDC ACM: support line request + send break */ \
    4, TUSB_DESC_CS_INTERFACE, CDC_FUNC_DESC_ABSTRACT_CONTROL_MANAGEMENT, 6, \
    /* CDC Union */ \
    5, TUSB_DESC_CS_INTERFACE, CDC_FUNC_DESC_UNION, _itfnum, (uint8_t)((_itfnum) + 1), \
    /* Endpoint Notification */ \
    7, TUSB_DESC_ENDPOINT, _ep_notif, TUSB_XFER_INTERRUPT, U16_TO_U8S_LE(VCOM_NOTIF_SIZE), VCOM_NOTIF_INTERVAL, \
    /* CDC Data Interface */ \
    9, TUSB_DESC_INTERFACE, (uint8_t)((_itfnum) + 1), 0, 2, TUSB_CLASS_CDC_DATA, 0, 0, 0, \
    /* Endpoint Out */ \
    7, TUSB_DESC_ENDPOINT, _epout, TUSB_XFER_BULK, U16_TO_U8S_LE(512), 0, \
    /* Endpoint In */ \
    7, TUSB_DESC_ENDPOINT, _epin, TUSB_XFER_BULK, U16_TO_U8S_LE(512), 0
// clang-format on

/** A vendor-defined report pair, 64 bytes each way: what CMSIS-DAP v1 is. */
static uint8_t const desc_hid_report[] = {
    TUD_HID_REPORT_DESC_GENERIC_INOUT(CFG_TUD_HID_EP_BUFSIZE),
};

uint8_t const* tud_hid_descriptor_report_cb(uint8_t instance) {
    (void)instance;
    return desc_hid_report;
}

#define CONFIG_MAX_LEN (TUD_CONFIG_DESC_LEN + TUD_VENDOR_DESC_LEN + TUD_HID_INOUT_DESC_LEN + 2U * TUD_CDC_DESC_LEN)

static uint8_t desc_configuration[CONFIG_MAX_LEN];

static uint8_t* append(uint8_t* out, uint8_t const* descriptor, size_t length) {
    memcpy(out, descriptor, length);
    return out + length;
}

/** Built on each request from the functions present, in PRO.md's interface order: CMSIS-DAP v2,
 *  VCOM, CMSIS-DAP v1, VCOM2. src/vcom.c numbers its CDC instances by the same rule. */
uint8_t const* tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    uint32_t const present = functions();
    uint8_t* out = desc_configuration + TUD_CONFIG_DESC_LEN;
    uint8_t itf = 0U;
    if ((present & FUNCTION_DEBUG) != 0U) {
        uint8_t const vendor[] = {
            TUD_VENDOR_DESCRIPTOR(ITF_NUM_VENDOR, STRID_VENDOR_ITF, EPNUM_VENDOR_OUT, EPNUM_VENDOR_IN, 512)};
        out = append(out, vendor, sizeof(vendor));
        itf++;
    }
    if ((present & FUNCTION_VCOM) != 0U) {
        uint8_t const vcom[] = {VCOM_DESCRIPTOR(itf, STRID_VCOM, EPNUM_VCOM_NOTIF, EPNUM_VCOM_OUT, EPNUM_VCOM_IN)};
        out = append(out, vcom, sizeof(vcom));
        itf += 2U;
    }
    if ((present & FUNCTION_DEBUG) != 0U) {
        uint8_t const hid[] = {TUD_HID_INOUT_DESCRIPTOR(itf, STRID_HID, HID_ITF_PROTOCOL_NONE, sizeof(desc_hid_report),
                                                        EPNUM_HID_OUT, EPNUM_HID_IN, CFG_TUD_HID_EP_BUFSIZE, 1)};
        out = append(out, hid, sizeof(hid));
        itf++;
    }
    if ((present & FUNCTION_VCOM2) != 0U) {
        uint8_t const vcom2[] = {VCOM_DESCRIPTOR(itf, STRID_VCOM2, EPNUM_VCOM2_NOTIF, EPNUM_VCOM2_OUT, EPNUM_VCOM2_IN)};
        out = append(out, vcom2, sizeof(vcom2));
        itf += 2U;
    }
    uint8_t const header[] = {TUD_CONFIG_DESCRIPTOR(1, itf, 0, (uint16_t)(out - desc_configuration), 0x00, 100)};
    (void)append(desc_configuration, header, sizeof(header));
    return desc_configuration;
}

// ---- Strings ------------------------------------------------------------------------------------

static char const* string_desc[] = {
    [0] = (const char[]){0x09, 0x04}, // English (US)
    [1] = DAP_VENDOR_NAME,
    [2] = NULL,                          // the product, which names the board: DAP_ProductName
    [3] = NULL,                          // serial, filled in from the part's unique id
    [STRID_VENDOR_ITF] = "CMSIS-DAP v2", // the prefix host tools match on
    [STRID_VCOM] = "Verge VCOM",
    [STRID_HID] = "CMSIS-DAP v1", // the same prefix, on the HID interface
    [STRID_VCOM2] = "Verge VCOM2",
};

static uint16_t string_buffer[32];

uint16_t const* tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    size_t count;

    if (index == 0) {
        memcpy(&string_buffer[1], string_desc[0], 2);
        count = 1;
    } else {
        if (index >= TU_ARRAY_SIZE(string_desc)) {
            return NULL;
        }
        char const* text = (index == 2)   ? DAP_ProductName(dap_shared.board)
                           : (index == 3) ? GetSerialNum()
                                          : string_desc[index];
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

#define MS_OS_20_DESC_LEN 178u
#define VENDOR_REQUEST_MICROSOFT 2

#define BOS_TOTAL_LEN (TUD_BOS_DESC_LEN + TUD_BOS_MICROSOFT_OS_DESC_LEN)

static uint8_t const desc_bos[] = {
    TUD_BOS_DESCRIPTOR(BOS_TOTAL_LEN, 1),
    TUD_BOS_MS_OS_20_DESCRIPTOR(MS_OS_20_DESC_LEN, VENDOR_REQUEST_MICROSOFT),
};

uint8_t const* tud_descriptor_bos_cb(void) { return ((functions() & FUNCTION_DEBUG) != 0U) ? desc_bos : NULL; }

// clang-format off: the layout mirrors the descriptor structure, which is the only thing that
// makes these bytes readable.
static uint8_t const desc_ms_os_20[] = {
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
// clang-format on

TU_VERIFY_STATIC(sizeof(desc_ms_os_20) == MS_OS_20_DESC_LEN, "MS OS 2.0 descriptor length mismatch");

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const* request) {
    if (request->bRequest == DAP_SHARED_CORE1_STATUS_REQUEST) {
        if (request->bmRequestType != 0xC0U || request->wValue > DAP_SHARED_SWO_STATUS || request->wIndex != 0U) {
            return false;
        }
        if (stage != CONTROL_STAGE_SETUP) {
            return true;
        }
        if (request->wValue == DAP_SHARED_QUEUE_STATUS) {
            // Slot count, the heads and tails, CPU 0's reply in flight, both mailbox registers'
            // low bytes, then each ring's lengths and the replies' transports: what
            // tools/dap.py core1 --queues prints.
            static uint8_t queues[11U + 7U * REQUEST_SLOTS];
            uint8_t* out = queues;
            *out++ = REQUEST_SLOTS;
            *out++ = dap_shared.request_head;
            *out++ = dap_shared.request_tail;
            *out++ = dap_shared.rx_armed_slot;
            *out++ = dap_shared.hid_request_head;
            *out++ = dap_shared.hid_request_tail;
            *out++ = dap_shared.response_head;
            *out++ = dap_shared.response_tail;
            *out++ = usb_transport_reply_in_flight() ? 1U : 0U;
            *out++ = (uint8_t)MAILBOX->MBOXIRQ[DAP_CORE_MAILBOX_CPU1].IRQ;
            *out++ = (uint8_t)MAILBOX->MBOXIRQ[DAP_CORE_MAILBOX_CPU0].IRQ;
            for (uint32_t i = 0U; i < REQUEST_SLOTS; i++) {
                uint16_t const lengths[] = {dap_shared.request_len[i], dap_shared.hid_request_len[i],
                                            dap_shared.response_len[i]};
                for (uint32_t j = 0U; j < 3U; j++) {
                    *out++ = (uint8_t)lengths[j];
                    *out++ = (uint8_t)(lengths[j] >> 8);
                }
                *out++ = dap_shared.response_transport[i];
            }
            return tud_control_xfer(rhport, request, queues, (uint16_t)TU_MIN(sizeof(queues), request->wLength));
        }
        if (request->wValue == DAP_SHARED_SWO_STATUS) {
            // Words: the segment count, DMA0's CTRL, ENABLESET, ACTIVE, BUSY, INTA and ERRINT, channel
            // 8's CFG, CTLSTAT and XFERCFG, Flexcomm 3's PSELID, the USART's CFG, STAT, FIFOCFG and
            // FIFOSTAT, then P0_3's IOCON word: what tools/dap.py core1 --swo prints. A peripheral
            // whose clock is off reads as zeros rather than being touched.
            static uint32_t swo[16];
            for (uint32_t i = 0U; i < 16U; i++) {
                swo[i] = 0U;
            }
            swo[0] = dap_shared.swo_segments;
            if ((SYSCON->AHBCLKCTRL.AHBCLKCTRL0 & SYSCON_AHBCLKCTRL0_DMA0_MASK) != 0U) {
                swo[1] = DMA0->CTRL;
                swo[2] = DMA0->COMMON[0].ENABLESET;
                swo[3] = DMA0->COMMON[0].ACTIVE;
                swo[4] = DMA0->COMMON[0].BUSY;
                swo[5] = DMA0->COMMON[0].INTA;
                swo[6] = DMA0->COMMON[0].ERRINT;
                swo[7] = DMA0->CHANNEL[SWO_DMA_CHANNEL].CFG;
                swo[8] = DMA0->CHANNEL[SWO_DMA_CHANNEL].CTLSTAT;
                swo[9] = DMA0->CHANNEL[SWO_DMA_CHANNEL].XFERCFG;
            }
            if ((SYSCON->AHBCLKCTRL.AHBCLKCTRL1 & SYSCON_AHBCLKCTRL1_FC3_MASK) != 0U) {
                swo[10] = FLEXCOMM3->PSELID;
                swo[11] = USART3->CFG;
                swo[12] = USART3->STAT;
                swo[13] = USART3->FIFOCFG;
                swo[14] = USART3->FIFOSTAT;
            }
            swo[15] = IOCON->PIO[0][3];
            return tud_control_xfer(rhport, request, swo, (uint16_t)TU_MIN(sizeof(swo), request->wLength));
        }
        // A snapshot, so the reply is one consistent read of what CPU 1 last wrote.
        static dap_core1_status_t status;
        status = dap_shared.core1;
        return tud_control_xfer(rhport, request, &status, (uint16_t)TU_MIN(sizeof(status), request->wLength));
    }

    if (request->bRequest == DAP_SHARED_CORE1_SAMPLE_REQUEST) {
        if (request->bmRequestType != 0x40U || request->wValue != 0U || request->wIndex != 0U ||
            request->wLength != 0U) {
            return false;
        }
        if (stage == CONTROL_STAGE_SETUP) {
            return tud_control_status(rhport, request);
        }
        if (stage == CONTROL_STAGE_ACK) {
            // The NMI mux takes CPU 0's interrupt lines, where the mailbox's is the register that
            // wakes CPU 0: raising CPU 1's own does nothing (measured). The NMI wants a rising edge,
            // so the line is dropped first.
            MAILBOX->MBOXIRQ[DAP_CORE_MAILBOX_CPU0].IRQCLR = UINT32_MAX;
            __DSB();
            SYSCON->NMISRC = SYSCON_NMISRC_IRQCPU1(MAILBOX_IRQn) | SYSCON_NMISRC_NMIENCPU1_MASK;
            __DSB();
            MAILBOX->MBOXIRQ[DAP_CORE_MAILBOX_CPU0].IRQSET = DAP_CORE_MAILBOX_WAKE_MASK;
        }
        return true;
    }

    if (request->bRequest == REBOOT_USB_VENDOR_REQUEST) {
        if (!reboot_usb_control_request_valid(request->bmRequestType, request->bRequest, request->wValue,
                                              request->wIndex, request->wLength)) {
            return false;
        }
        if (stage == CONTROL_STAGE_SETUP) {
            return tud_control_status(rhport, request);
        }
        if (stage == CONTROL_STAGE_ACK) {
            reboot_request_from_usb((uint8_t)request->wValue);
        }
        return true;
    }

    if (stage != CONTROL_STAGE_SETUP) {
        return true;
    }
    if (request->bmRequestType_bit.type == TUSB_REQ_TYPE_VENDOR && request->bRequest == VENDOR_REQUEST_MICROSOFT &&
        request->wIndex == 7) {
        uint16_t total_len;
        memcpy(&total_len, desc_ms_os_20 + 8, 2);
        return tud_control_xfer(rhport, request, (void*)(uintptr_t)desc_ms_os_20, total_len);
    }
    return false;
}
