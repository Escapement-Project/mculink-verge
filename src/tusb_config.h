#ifndef TUSB_CONFIG_H
#define TUSB_CONFIG_H

#ifndef BOARD_TUD_RHPORT
#define BOARD_TUD_RHPORT 1
#endif

// The stock firmware reports a 512 byte packet size, which is high-speed bulk. Matching it keeps
// the host's packet accounting the same as it is today.
#ifndef BOARD_TUD_MAX_SPEED
#define BOARD_TUD_MAX_SPEED OPT_MODE_HIGH_SPEED
#endif

#define CFG_TUSB_MCU OPT_MCU_LPC55
#define CFG_TUSB_OS OPT_OS_NONE
#define CFG_TUD_ENABLED 1
#define CFG_TUD_MAX_SPEED BOARD_TUD_MAX_SPEED

// CFG_TUSB_MEM_SECTION and CFG_TUSB_MEM_ALIGN come from the BSP on the command line, because the
// LPC55 USB controller needs its buffers in a particular region and alignment.

#define CFG_TUD_ENDPOINT0_SIZE 64

#define CFG_TUD_CDC 1

// The target console. 512 each way matches the bulk packet size, so a full USB packet never has to
// be split across a FIFO wrap.
#define CFG_TUD_CDC_RX_BUFSIZE 512
#define CFG_TUD_CDC_TX_BUFSIZE 512
#define CFG_TUD_CDC_EP_BUFSIZE 512
#define CFG_TUD_MSC 0
#define CFG_TUD_HID 1

// CMSIS-DAP v1 carries one command per HID report, and a report is capped by the interrupt
// endpoint. 64 is the size every v1 host expects.
#define CFG_TUD_HID_EP_BUFSIZE 64
#define CFG_TUD_MIDI 0
#define CFG_TUD_VENDOR 1

// One DAP packet each way, so a full packet never has to be split across FIFO wraps.
// **No FIFO, because CMSIS-DAP frames one command per packet and a FIFO does not keep frames.**
// Zero on either buffer selects TinyUSB's non-buffered mode: a received packet arrives at
// tud_vendor_rx_cb with its own length, and tud_vendor_write turns into one usbd_edpt_xfer. In
// the buffered mode the class is a byte stream -- tud_vendor_read drains whatever is there, so
// two small commands arriving together are handed to one read and the second is answered by
// nobody. That was measured: the host times out from three commands in flight upward.
#define CFG_TUD_VENDOR_RX_BUFSIZE 0
#define CFG_TUD_VENDOR_TX_BUFSIZE 0
#define CFG_TUD_VENDOR_EPSIZE 512
// The firmware arms the receive endpoint itself, which is how it refuses a command it has no
// slot for. src/main.c holds the slots.
#define CFG_TUD_VENDOR_RX_MANUAL_XFER 1

#endif
